#include "common/linux/image_utils.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace nativekit::platform {
namespace {

constexpr std::size_t kMaximumDataUrlLength = 32 * 1024 * 1024;
constexpr std::uint64_t kMaximumDecodedBytes = 64 * 1024 * 1024;
constexpr int kMaximumImageDimension = 8192;

template <typename Type>
struct GObjectDeleter {
  void operator()(Type* value) const {
    if (value != nullptr) g_object_unref(value);
  }
};

using AppInfoPtr = std::unique_ptr<GAppInfo, GObjectDeleter<GAppInfo>>;
using FileInfoPtr = std::unique_ptr<GFileInfo, GObjectDeleter<GFileInfo>>;
using FilePtr = std::unique_ptr<GFile, GObjectDeleter<GFile>>;
using InputStreamPtr =
    std::unique_ptr<GInputStream, GObjectDeleter<GInputStream>>;
using PixbufPtr = std::unique_ptr<GdkPixbuf, GObjectDeleter<GdkPixbuf>>;
using PixbufLoaderPtr =
    std::unique_ptr<GdkPixbufLoader, GObjectDeleter<GdkPixbufLoader>>;

struct GErrorDeleter {
  void operator()(GError* error) const {
    if (error != nullptr) g_error_free(error);
  }
};

using ErrorPtr = std::unique_ptr<GError, GErrorDeleter>;

std::runtime_error image_error(const char* operation, GError* error) {
  return std::runtime_error(
      std::string(operation) + " failed" +
      (error == nullptr ? "" : std::string(": ") + error->message));
}

std::string lower_ascii(std::string value) {
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return value;
}

bool valid_base64(const std::string& value) {
  if (value.empty() || value.size() % 4 != 0) return false;
  const std::size_t padding_start = value.find('=');
  if (padding_start != std::string::npos) {
    const std::size_t padding = value.size() - padding_start;
    if (padding > 2 ||
        value.find_first_not_of('=', padding_start) != std::string::npos) {
      return false;
    }
  }
  const std::size_t content_size =
      padding_start == std::string::npos ? value.size() : padding_start;
  for (std::size_t index = 0; index < content_size; ++index) {
    const unsigned char character = value[index];
    if (!(std::isalnum(character) || character == '+' || character == '/')) {
      return false;
    }
  }
  return true;
}

struct PreparedImageSize {
  bool invalid = false;
};

void validate_prepared_size(
    GdkPixbufLoader* loader,
    int width,
    int height,
    gpointer user_data) {
  auto& state = *static_cast<PreparedImageSize*>(user_data);
  const std::uint64_t pixel_count =
      width > 0 && height > 0
          ? static_cast<std::uint64_t>(width) * height
          : 0;
  state.invalid = width <= 0 || height <= 0 ||
                  width > kMaximumImageDimension ||
                  height > kMaximumImageDimension ||
                  pixel_count > kMaximumDecodedBytes / 4;
  if (state.invalid) {
    // Prevent allocation after an unsafe source size is exposed.
    gdk_pixbuf_loader_set_size(loader, 1, 1);
  }
}

PixbufPtr exact_square_pixbuf(GdkPixbuf* source, int pixels) {
  if (source == nullptr || pixels <= 0) return {};
  const int source_width = gdk_pixbuf_get_width(source);
  const int source_height = gdk_pixbuf_get_height(source);
  if (source_width <= 0 || source_height <= 0) return {};

  const double scale = std::min(
      static_cast<double>(pixels) / source_width,
      static_cast<double>(pixels) / source_height);
  const int width = std::clamp(
      static_cast<int>(std::lround(source_width * scale)), 1, pixels);
  const int height = std::clamp(
      static_cast<int>(std::lround(source_height * scale)), 1, pixels);
  PixbufPtr scaled(gdk_pixbuf_scale_simple(
      source, width, height, GDK_INTERP_BILINEAR));
  if (!scaled) return {};

  PixbufPtr result(gdk_pixbuf_new(
      GDK_COLORSPACE_RGB, TRUE, 8, pixels, pixels));
  if (!result) return {};
  gdk_pixbuf_fill(result.get(), 0x00000000);
  gdk_pixbuf_copy_area(
      scaled.get(),
      0,
      0,
      width,
      height,
      result.get(),
      (pixels - width) / 2,
      (pixels - height) / 2);
  return result;
}

std::filesystem::path canonical_path(const std::string& value) {
  std::error_code error;
  const auto path = std::filesystem::path(value);
  auto result = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : result;
}

bool executable_matches(
    const std::filesystem::path& requested,
    const char* executable) {
  if (executable == nullptr || *executable == '\0') return false;
  const std::filesystem::path candidate(executable);
  if (candidate.is_absolute() && canonical_path(candidate.string()) == requested) {
    return true;
  }
  return candidate.filename() == requested.filename();
}

AppInfoPtr app_info_for_path(const std::string& app_path) {
  const std::filesystem::path requested = canonical_path(app_path);
  if (requested.extension() == ".desktop") {
    return AppInfoPtr(G_APP_INFO(
        g_desktop_app_info_new_from_filename(requested.c_str())));
  }

  GList* values = g_app_info_get_all();
  GAppInfo* match = nullptr;
  for (GList* cursor = values; cursor != nullptr; cursor = cursor->next) {
    auto* info = G_APP_INFO(cursor->data);
    if (executable_matches(requested, g_app_info_get_executable(info))) {
      match = G_APP_INFO(g_object_ref(info));
      break;
    }
  }
  g_list_free_full(values, g_object_unref);
  return AppInfoPtr(match);
}

void append_unique(
    std::vector<std::filesystem::path>& values,
    const std::filesystem::path& value) {
  if (value.empty() ||
      std::find(values.begin(), values.end(), value) != values.end()) {
    return;
  }
  values.push_back(value);
}

void append_unique(std::vector<std::string>& values, std::string value) {
  if (value.empty() ||
      std::find(values.begin(), values.end(), value) != values.end()) {
    return;
  }
  values.push_back(std::move(value));
}

std::vector<std::filesystem::path> icon_roots() {
  std::vector<std::filesystem::path> result;
  const char* home = std::getenv("HOME");
  const char* data_home = std::getenv("XDG_DATA_HOME");
  if (data_home != nullptr && *data_home != '\0') {
    append_unique(result, std::filesystem::path(data_home) / "icons");
  } else if (home != nullptr && *home != '\0') {
    append_unique(
        result, std::filesystem::path(home) / ".local/share/icons");
  }
  if (home != nullptr && *home != '\0') {
    append_unique(result, std::filesystem::path(home) / ".icons");
  }

  const char* raw_directories = std::getenv("XDG_DATA_DIRS");
  const std::string directories =
      raw_directories == nullptr || *raw_directories == '\0'
          ? "/usr/local/share:/usr/share"
          : raw_directories;
  std::size_t offset = 0;
  while (offset <= directories.size()) {
    const std::size_t separator = directories.find(':', offset);
    const std::string value = directories.substr(
        offset,
        separator == std::string::npos
            ? std::string::npos
            : separator - offset);
    if (!value.empty()) {
      append_unique(result, std::filesystem::path(value) / "icons");
    }
    if (separator == std::string::npos) break;
    offset = separator + 1;
  }
  return result;
}

std::vector<std::string> installed_themes(
    const std::vector<std::filesystem::path>& roots) {
  std::vector<std::string> result;
  const char* gtk_theme = std::getenv("GTK_THEME");
  if (gtk_theme != nullptr && *gtk_theme != '\0') {
    std::string value(gtk_theme);
    const std::size_t variant = value.find(':');
    if (variant != std::string::npos) value.erase(variant);
    append_unique(result, std::move(value));
  }
  for (const char* fallback : {
           "hicolor", "Adwaita", "Yaru", "breeze", "Humanity"}) {
    append_unique(result, fallback);
  }
  for (const auto& root : roots) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
      if (iterator->is_directory(error)) {
        append_unique(result, iterator->path().filename().string());
      }
      iterator.increment(error);
    }
  }
  return result;
}

std::optional<std::filesystem::path> existing_icon_path(
    const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error
      ? std::optional<std::filesystem::path>(path)
      : std::nullopt;
}

std::optional<std::filesystem::path> find_themed_icon(
    const std::vector<std::string>& names,
    int pixels) {
  const auto roots = icon_roots();
  const auto themes = installed_themes(roots);
  const std::vector<std::string> sizes = {
      std::to_string(pixels) + "x" + std::to_string(pixels),
      "16x16", "22x22", "24x24", "32x32", "48x48", "64x64",
      "128x128", "256x256", "512x512", "scalable", "symbolic",
  };
  const std::vector<std::string> contexts = {
      "apps", "mimetypes", "places", "status", "categories",
  };
  const std::vector<std::string> extensions = {"png", "svg", "xpm"};

  for (const auto& name_value : names) {
    const std::filesystem::path original(name_value);
    if (original.is_absolute()) {
      if (const auto match = existing_icon_path(original)) return match;
    }
    std::string name = original.filename().string();
    const std::string extension = lower_ascii(original.extension().string());
    if (extension == ".png" || extension == ".svg" || extension == ".xpm") {
      name = original.stem().string();
    }
    for (const auto& root : roots) {
      for (const auto& theme : themes) {
        for (const auto& size : sizes) {
          for (const auto& context : contexts) {
            for (const auto& candidate_extension : extensions) {
              if (const auto match = existing_icon_path(
                      root / theme / size / context /
                      (name + "." + candidate_extension))) {
                return match;
              }
            }
          }
        }
      }
    }
    for (const char* pixmap_root : {"/usr/local/share/pixmaps", "/usr/share/pixmaps"}) {
      for (const auto& candidate_extension : extensions) {
        if (const auto match = existing_icon_path(
                std::filesystem::path(pixmap_root) /
                (name + "." + candidate_extension))) {
          return match;
        }
      }
    }
  }
  return std::nullopt;
}

PixbufPtr pixbuf_from_file(
    const std::filesystem::path& path,
    int pixels) {
  GError* raw_error = nullptr;
  PixbufPtr result(gdk_pixbuf_new_from_file_at_scale(
      path.c_str(), pixels, pixels, TRUE, &raw_error));
  ErrorPtr error(raw_error);
  return result;
}

PixbufPtr pixbuf_from_loadable_icon(GIcon* icon, int pixels) {
  if (icon == nullptr || !G_IS_LOADABLE_ICON(icon)) return {};
  GError* raw_error = nullptr;
  InputStreamPtr stream(g_loadable_icon_load(
      G_LOADABLE_ICON(icon), pixels, nullptr, nullptr, &raw_error));
  ErrorPtr load_error(raw_error);
  if (!stream) return {};
  raw_error = nullptr;
  PixbufPtr pixbuf(gdk_pixbuf_new_from_stream_at_scale(
      stream.get(), pixels, pixels, TRUE, nullptr, &raw_error));
  ErrorPtr decode_error(raw_error);
  return pixbuf;
}

std::vector<std::string> icon_names(GIcon* icon) {
  std::vector<std::string> result;
  if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
    const gchar* const* names =
        g_themed_icon_get_names(G_THEMED_ICON(icon));
    for (std::size_t index = 0;
         names != nullptr && names[index] != nullptr;
         ++index) {
      append_unique(result, names[index]);
    }
  }
  return result;
}

PixbufPtr pixbuf_for_icon(GIcon* icon, int pixels) {
  if (PixbufPtr loadable = pixbuf_from_loadable_icon(icon, pixels)) {
    return loadable;
  }
  const auto names = icon_names(icon);
  const auto path = find_themed_icon(names, pixels);
  return path ? pixbuf_from_file(*path, pixels) : PixbufPtr{};
}

PixbufPtr file_icon_pixbuf(const std::string& app_path, int pixels) {
  FilePtr file(g_file_new_for_path(app_path.c_str()));
  if (!file) return {};
  GError* raw_error = nullptr;
  FileInfoPtr info(g_file_query_info(
      file.get(),
      G_FILE_ATTRIBUTE_STANDARD_ICON,
      G_FILE_QUERY_INFO_NONE,
      nullptr,
      &raw_error));
  ErrorPtr error(raw_error);
  if (!info) return {};
  return pixbuf_for_icon(g_file_info_get_icon(info.get()), pixels);
}

PixbufPtr generic_icon_pixbuf(int pixels) {
  const auto path = find_themed_icon(
      {"application-x-executable", "application-x-generic", "application-default-icon"},
      pixels);
  if (path) {
    if (PixbufPtr themed = pixbuf_from_file(*path, pixels)) return themed;
  }
  PixbufPtr fallback(gdk_pixbuf_new(
      GDK_COLORSPACE_RGB, TRUE, 8, pixels, pixels));
  if (fallback) gdk_pixbuf_fill(fallback.get(), 0x3584e4ff);
  return fallback;
}

}  // namespace

GdkPixbuf* pixbuf_from_data_url(const std::string& data_url) {
  if (data_url.empty() || data_url.size() > kMaximumDataUrlLength) {
    throw std::runtime_error("overlay image data exceeds the 32 MiB limit");
  }
  const std::size_t comma = data_url.find(',');
  if (comma == std::string::npos || comma + 1 >= data_url.size()) {
    throw std::runtime_error("overlay image data URL is invalid");
  }
  const std::string header = lower_ascii(data_url.substr(0, comma));
  if (header != "data:image/png;base64" &&
      header != "data:image/jpeg;base64" &&
      header != "data:image/jpg;base64") {
    throw std::runtime_error("overlay image must be a PNG or JPEG data URL");
  }
  const std::string encoded = data_url.substr(comma + 1);
  if (!valid_base64(encoded)) {
    throw std::runtime_error("overlay image base64 data is invalid");
  }

  gsize decoded_size = 0;
  std::unique_ptr<guchar, decltype(&g_free)> decoded(
      g_base64_decode(encoded.c_str(), &decoded_size), &g_free);
  if (!decoded || decoded_size == 0) {
    throw std::runtime_error("overlay image base64 data is invalid");
  }

  PixbufLoaderPtr loader(gdk_pixbuf_loader_new());
  if (!loader) throw std::runtime_error("GdkPixbuf loader is unavailable");
  PreparedImageSize prepared;
  g_signal_connect(
      loader.get(),
      "size-prepared",
      G_CALLBACK(validate_prepared_size),
      &prepared);

  GError* raw_error = nullptr;
  const bool written = gdk_pixbuf_loader_write(
      loader.get(), decoded.get(), decoded_size, &raw_error);
  ErrorPtr error(raw_error);
  if (!written) throw image_error("GdkPixbuf decode", error.get());

  raw_error = nullptr;
  const bool closed = gdk_pixbuf_loader_close(loader.get(), &raw_error);
  ErrorPtr close_error(raw_error);
  if (!closed) throw image_error("GdkPixbuf decode", close_error.get());
  if (prepared.invalid) {
    throw std::runtime_error("overlay image dimensions exceed the limit");
  }

  GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader.get());
  if (pixbuf == nullptr) {
    throw std::runtime_error("overlay image data could not be decoded");
  }
  return GDK_PIXBUF(g_object_ref(pixbuf));
}

GdkPixbuf* app_icon_pixbuf(const std::string& app_path, int pixels) {
  if (pixels <= 0 || app_path.empty()) return nullptr;
  std::error_code error;
  if (!std::filesystem::exists(app_path, error) || error) return nullptr;

  PixbufPtr source;
  if (AppInfoPtr info = app_info_for_path(app_path)) {
    source = pixbuf_for_icon(g_app_info_get_icon(info.get()), pixels);
  }
  if (!source) source = file_icon_pixbuf(app_path, pixels);
  if (!source) source = generic_icon_pixbuf(pixels);
  PixbufPtr exact = exact_square_pixbuf(source.get(), pixels);
  return exact ? exact.release() : nullptr;
}

std::optional<std::string> pixbuf_to_png_data_url(
    GdkPixbuf* pixbuf,
    int pixels) {
  PixbufPtr exact = exact_square_pixbuf(pixbuf, pixels);
  if (!exact) return std::nullopt;

  gchar* raw_png = nullptr;
  gsize png_size = 0;
  GError* raw_error = nullptr;
  const bool saved = gdk_pixbuf_save_to_buffer(
      exact.get(),
      &raw_png,
      &png_size,
      "png",
      &raw_error,
      nullptr);
  std::unique_ptr<gchar, decltype(&g_free)> png(raw_png, &g_free);
  ErrorPtr error(raw_error);
  if (!saved || !png || png_size == 0) return std::nullopt;

  std::unique_ptr<gchar, decltype(&g_free)> encoded(
      g_base64_encode(
          reinterpret_cast<const guchar*>(png.get()), png_size),
      &g_free);
  if (!encoded) return std::nullopt;
  return std::string("data:image/png;base64,") + encoded.get();
}

std::optional<std::string> icon_to_png_data_url(
    const std::string& app_path,
    int pixels) {
  PixbufPtr pixbuf(app_icon_pixbuf(app_path, pixels));
  return pixbuf_to_png_data_url(pixbuf.get(), pixels);
}

}  // namespace nativekit::platform
