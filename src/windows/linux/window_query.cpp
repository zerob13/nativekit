#include "windows/window_query.h"

#include <xcb/xcb.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/linux/image_utils.h"

namespace nativekit::platform {
namespace {

template <typename Type>
struct FreeDeleter {
  void operator()(Type* value) const { std::free(value); }
};

template <typename Type>
using ReplyPtr = std::unique_ptr<Type, FreeDeleter<Type>>;

class XcbConnection {
 public:
  XcbConnection() {
    int screen_number = 0;
    connection_ = xcb_connect(nullptr, &screen_number);
    if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
      if (connection_ != nullptr) xcb_disconnect(connection_);
      connection_ = nullptr;
      throw std::runtime_error(
          "nativekit Linux window APIs require an available X11 display");
    }

    const xcb_setup_t* setup = xcb_get_setup(connection_);
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
    for (int index = 0; iterator.rem != 0 && index < screen_number; ++index) {
      xcb_screen_next(&iterator);
    }
    screen_ = iterator.data;
    if (screen_ == nullptr) {
      xcb_disconnect(connection_);
      connection_ = nullptr;
      throw std::runtime_error("X11 did not expose a default screen");
    }
  }

  XcbConnection(const XcbConnection&) = delete;
  XcbConnection& operator=(const XcbConnection&) = delete;
  ~XcbConnection() {
    if (connection_ != nullptr) xcb_disconnect(connection_);
  }

  [[nodiscard]] xcb_connection_t* get() const { return connection_; }
  [[nodiscard]] const xcb_screen_t& screen() const { return *screen_; }

 private:
  xcb_connection_t* connection_ = nullptr;
  xcb_screen_t* screen_ = nullptr;
};

class Atoms {
 public:
  explicit Atoms(xcb_connection_t* connection) : connection_(connection) {}

  xcb_atom_t get(const char* name) {
    const auto existing = values_.find(name);
    if (existing != values_.end()) return existing->second;
    const std::size_t length = std::char_traits<char>::length(name);
    const auto cookie = xcb_intern_atom(
        connection_, true, static_cast<std::uint16_t>(length), name);
    ReplyPtr<xcb_intern_atom_reply_t> reply(
        xcb_intern_atom_reply(connection_, cookie, nullptr));
    const xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    values_.emplace(name, atom);
    return atom;
  }

 private:
  xcb_connection_t* connection_;
  std::unordered_map<std::string, xcb_atom_t> values_;
};

std::vector<std::uint32_t> property32(
    xcb_connection_t* connection,
    xcb_window_t window,
    xcb_atom_t property,
    xcb_atom_t expected_type = XCB_GET_PROPERTY_TYPE_ANY) {
  if (property == XCB_ATOM_NONE) return {};
  const auto cookie = xcb_get_property(
      connection,
      false,
      window,
      property,
      expected_type,
      0,
      1U << 20U);
  ReplyPtr<xcb_get_property_reply_t> reply(
      xcb_get_property_reply(connection, cookie, nullptr));
  if (!reply || reply->format != 32 ||
      (expected_type != XCB_GET_PROPERTY_TYPE_ANY &&
       reply->type != expected_type)) {
    return {};
  }
  const int byte_length = xcb_get_property_value_length(reply.get());
  if (byte_length <= 0 || byte_length % 4 != 0) return {};
  const auto* values = static_cast<const std::uint32_t*>(
      xcb_get_property_value(reply.get()));
  return {values, values + byte_length / 4};
}

std::optional<std::string> string_property(
    xcb_connection_t* connection,
    xcb_window_t window,
    xcb_atom_t property,
    xcb_atom_t expected_type = XCB_GET_PROPERTY_TYPE_ANY) {
  if (property == XCB_ATOM_NONE) return std::nullopt;
  const auto cookie = xcb_get_property(
      connection,
      false,
      window,
      property,
      expected_type,
      0,
      16384);
  ReplyPtr<xcb_get_property_reply_t> reply(
      xcb_get_property_reply(connection, cookie, nullptr));
  if (!reply || reply->format != 8 ||
      (expected_type != XCB_GET_PROPERTY_TYPE_ANY &&
       reply->type != expected_type)) {
    return std::nullopt;
  }
  const int length = xcb_get_property_value_length(reply.get());
  if (length <= 0) return std::nullopt;
  const auto* value = static_cast<const char*>(
      xcb_get_property_value(reply.get()));
  const auto end = std::find(value, value + length, '\0');
  if (end == value) return std::nullopt;
  return std::string(value, end);
}

std::string process_name(std::uint32_t pid) {
  if (pid == 0) return {};
  std::ifstream stream("/proc/" + std::to_string(pid) + "/comm");
  std::string name;
  std::getline(stream, name);
  return name;
}

std::string executable_path(std::uint32_t pid) {
  if (pid == 0) return {};
  std::error_code error;
  const auto path = std::filesystem::read_symlink(
      "/proc/" + std::to_string(pid) + "/exe", error);
  return error ? std::string() : path.string();
}

std::vector<xcb_window_t> stacking_order(
    XcbConnection& connection,
    Atoms& atoms) {
  std::vector<std::uint32_t> values = property32(
      connection.get(),
      connection.screen().root,
      atoms.get("_NET_CLIENT_LIST_STACKING"),
      XCB_ATOM_WINDOW);
  std::vector<xcb_window_t> windows(values.begin(), values.end());
  if (windows.empty()) {
    const auto cookie =
        xcb_query_tree(connection.get(), connection.screen().root);
    ReplyPtr<xcb_query_tree_reply_t> reply(
        xcb_query_tree_reply(connection.get(), cookie, nullptr));
    if (reply) {
      const int length = xcb_query_tree_children_length(reply.get());
      const xcb_window_t* children = xcb_query_tree_children(reply.get());
      if (length > 0 && children != nullptr) {
        windows.assign(children, children + length);
      }
    }
  }
  std::reverse(windows.begin(), windows.end());
  return windows;
}

bool has_state(
    xcb_connection_t* connection,
    Atoms& atoms,
    xcb_window_t window,
    const char* state_name) {
  const xcb_atom_t target = atoms.get(state_name);
  if (target == XCB_ATOM_NONE) return false;
  const auto states = property32(
      connection,
      window,
      atoms.get("_NET_WM_STATE"),
      XCB_ATOM_ATOM);
  return std::find(states.begin(), states.end(), target) != states.end();
}

std::optional<SystemWindow> parse_window(
    XcbConnection& connection,
    Atoms& atoms,
    xcb_window_t id,
    int level) {
  const auto geometry_cookie = xcb_get_geometry(connection.get(), id);
  const auto attributes_cookie =
      xcb_get_window_attributes(connection.get(), id);
  const auto translate_cookie = xcb_translate_coordinates(
      connection.get(), id, connection.screen().root, 0, 0);
  ReplyPtr<xcb_get_geometry_reply_t> geometry(
      xcb_get_geometry_reply(connection.get(), geometry_cookie, nullptr));
  ReplyPtr<xcb_get_window_attributes_reply_t> attributes(
      xcb_get_window_attributes_reply(
          connection.get(), attributes_cookie, nullptr));
  ReplyPtr<xcb_translate_coordinates_reply_t> translated(
      xcb_translate_coordinates_reply(
          connection.get(), translate_cookie, nullptr));
  if (!geometry || !attributes || !translated ||
      geometry->width == 0 || geometry->height == 0 ||
      attributes->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
    return std::nullopt;
  }

  SystemWindow result;
  result.id = id;
  result.name = string_property(
      connection.get(),
      id,
      atoms.get("_NET_WM_NAME"),
      atoms.get("UTF8_STRING"));
  if (!result.name) {
    result.name = string_property(
        connection.get(), id, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
  }
  result.bounds = {
      static_cast<double>(translated->dst_x),
      static_cast<double>(translated->dst_y),
      static_cast<double>(geometry->width),
      static_cast<double>(geometry->height),
  };
  result.level = level;
  const auto pid_values = property32(
      connection.get(),
      id,
      atoms.get("_NET_WM_PID"),
      XCB_ATOM_CARDINAL);
  if (!pid_values.empty()) result.owner_pid = pid_values.front();
  const std::string owner = process_name(result.owner_pid);
  if (!owner.empty()) result.owner_name = owner;

  const double root_width = connection.screen().width_in_pixels;
  const double root_height = connection.screen().height_in_pixels;
  const bool intersects_root =
      result.bounds.x < root_width && result.bounds.y < root_height &&
      result.bounds.x + result.bounds.width > 0 &&
      result.bounds.y + result.bounds.height > 0;
  result.is_onscreen =
      attributes->map_state == XCB_MAP_STATE_VIEWABLE && intersects_root &&
      !has_state(connection.get(), atoms, id, "_NET_WM_STATE_HIDDEN");
  return result;
}

std::vector<SystemWindow> all_windows(
    XcbConnection& connection,
    Atoms& atoms) {
  const auto order = stacking_order(connection, atoms);
  std::vector<SystemWindow> windows;
  windows.reserve(order.size());
  for (xcb_window_t id : order) {
    if (auto window = parse_window(
            connection, atoms, id, static_cast<int>(windows.size()))) {
      windows.push_back(std::move(*window));
    }
  }
  return windows;
}

std::vector<SystemWindow> windows_below(
    std::vector<SystemWindow> windows,
    WindowId relative_to) {
  if (relative_to == 0) return windows;
  const auto reference = std::find_if(
      windows.begin(),
      windows.end(),
      [relative_to](const SystemWindow& window) {
        return window.id == relative_to;
      });
  if (reference == windows.end()) return {};
  windows.erase(windows.begin(), std::next(reference));
  return windows;
}

}  // namespace

std::vector<SystemWindow> list_windows(WindowId relative_to) {
  XcbConnection connection;
  Atoms atoms(connection.get());
  return windows_below(all_windows(connection, atoms), relative_to);
}

std::optional<SystemWindow> find_window(WindowId id) {
  XcbConnection connection;
  Atoms atoms(connection.get());
  const auto windows = all_windows(connection, atoms);
  const auto match = std::find_if(
      windows.begin(), windows.end(), [id](const SystemWindow& window) {
        return window.id == id;
      });
  return match == windows.end() ? std::nullopt : std::optional(*match);
}

std::optional<FrontmostWindow> frontmost_window() {
  XcbConnection connection;
  Atoms atoms(connection.get());
  const auto active = property32(
      connection.get(),
      connection.screen().root,
      atoms.get("_NET_ACTIVE_WINDOW"),
      XCB_ATOM_WINDOW);
  if (active.empty() || active.front() == XCB_WINDOW_NONE) {
    return std::nullopt;
  }

  const auto windows = all_windows(connection, atoms);
  const auto match = std::find_if(
      windows.begin(), windows.end(), [&active](const SystemWindow& window) {
        return window.id == active.front();
      });
  if (match == windows.end()) return std::nullopt;

  const std::string path = executable_path(match->owner_pid);
  FrontmostWindow result;
  result.bundle_id = path;
  result.name = match->owner_name.value_or(
      path.empty() ? match->name.value_or("")
                   : std::filesystem::path(path).filename().string());
  result.title = match->name;
  if (!path.empty()) result.icon = icon_to_png_data_url(path, 32);
  return result;
}

std::optional<SystemWindow> window_at_point(Point point, WindowId below_id) {
  const auto windows = list_windows(below_id);
  const auto match = std::find_if(
      windows.begin(), windows.end(), [&point](const SystemWindow& window) {
        return window.is_onscreen && window.bounds.contains(point);
      });
  return match == windows.end() ? std::nullopt : std::optional(*match);
}

}  // namespace nativekit::platform
