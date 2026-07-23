#include "overlay/overlay_manager.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/linux/image_utils.h"

namespace nativekit::platform {
namespace {

constexpr int kMinimumPanelSize = 64;
constexpr int kStackGap = 12;
constexpr int kControlMargin = 6;
constexpr int kControlGap = 4;
constexpr int kControlSize = 24;
constexpr int kControlStroke = 2;
constexpr int kIconMargin = 8;
constexpr int kIconSize = 28;
constexpr std::uint32_t kDoubleClickMilliseconds = 400;

template <typename Type>
struct GObjectDeleter {
  void operator()(Type* value) const {
    if (value != nullptr) g_object_unref(value);
  }
};

using PixbufPtr = std::unique_ptr<GdkPixbuf, GObjectDeleter<GdkPixbuf>>;

struct RectI {
  int x = 0;
  int y = 0;
  int width = 1;
  int height = 1;
};

enum class Control { kNone, kHide, kRelocate };

struct ControlRects {
  RectI hide;
  RectI relocate;
};

struct PanelState {
  std::string id;
  std::string host_id;
  xcb_window_t window = XCB_WINDOW_NONE;
  xcb_pixmap_t background = XCB_PIXMAP_NONE;
  RectI work_area;
  int x = 0;
  int y = 0;
  int width = 1;
  int height = 1;
  int manual_x = 0;
  int manual_y = 0;
  int drag_start_root_x = 0;
  int drag_start_root_y = 0;
  int drag_start_x = 0;
  int drag_start_y = 0;
  std::uint32_t last_click_time = 0;
  Control pressed_control = Control::kNone;
  bool dragging = false;
  bool drag_moved = false;
  bool manually_positioned = false;
  bool visible = false;
};

struct RenderItem {
  std::string id;
  std::string host_id;
  std::string title;
  xcb_window_t host_window = XCB_WINDOW_NONE;
  PixbufPtr image;
  RectI work_area;
  int x = 0;
  int y = 0;
  int width = 1;
  int height = 1;
  bool visible = false;
  bool preserve_position = false;
};

struct HostLayout {
  RectI work_area;
  double cursor = 0;
};

struct PixelFormat {
  std::uint8_t depth = 0;
  std::uint8_t bits_per_pixel = 0;
  std::uint8_t scanline_pad = 0;
  std::uint8_t image_byte_order = XCB_IMAGE_ORDER_LSB_FIRST;
  std::uint32_t red_mask = 0;
  std::uint32_t green_mask = 0;
  std::uint32_t blue_mask = 0;
};

struct Atoms {
  xcb_atom_t utf8_string = XCB_ATOM_NONE;
  xcb_atom_t net_wm_name = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type = XCB_ATOM_NONE;
  xcb_atom_t net_wm_window_type_utility = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_above = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_skip_taskbar = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_skip_pager = XCB_ATOM_NONE;
  xcb_atom_t net_wm_state_sticky = XCB_ATOM_NONE;
  xcb_atom_t net_wm_desktop = XCB_ATOM_NONE;
  xcb_atom_t net_workarea = XCB_ATOM_NONE;
  xcb_atom_t net_current_desktop = XCB_ATOM_NONE;
  xcb_atom_t motif_wm_hints = XCB_ATOM_NONE;
};

ControlRects control_rects(int width, int height) {
  const int margin = std::max(1, std::min({kControlMargin, width / 8, height / 8}));
  const int gap = std::max(1, std::min(kControlGap, width / 16));
  const int available_width = std::max(2, width - margin * 2 - gap);
  const int button = std::max(
      1,
      std::min({kControlSize, height - margin * 2, available_width / 2}));
  ControlRects result;
  result.hide = {width - margin - button, margin, button, button};
  result.relocate = {
      result.hide.x - gap - button, margin, button, button};
  return result;
}

bool contains(const RectI& rect, int x, int y) {
  return x >= rect.x && y >= rect.y &&
         x < rect.x + rect.width && y < rect.y + rect.height;
}

Control hit_test_control(const PanelState& state, int x, int y) {
  const ControlRects controls = control_rects(state.width, state.height);
  if (contains(controls.hide, x, y)) return Control::kHide;
  if (contains(controls.relocate, x, y)) return Control::kRelocate;
  return Control::kNone;
}

RectI intersect_rects(const RectI& first, const RectI& second) {
  const int left = std::max(first.x, second.x);
  const int top = std::max(first.y, second.y);
  const int right = std::min(
      first.x + first.width, second.x + second.width);
  const int bottom = std::min(
      first.y + first.height, second.y + second.height);
  return {
      left,
      top,
      std::max(0, right - left),
      std::max(0, bottom - top),
  };
}

std::pair<int, int> fitted_size(
    GdkPixbuf* image,
    const OverlayHost& host,
    double max_size,
    const RectI& work_area) {
  const int source_width = gdk_pixbuf_get_width(image);
  const int source_height = gdk_pixbuf_get_height(image);
  const double width_limit = std::min({
      max_size,
      std::max(host.bounds.width, static_cast<double>(kMinimumPanelSize)),
      static_cast<double>(work_area.width),
  });
  const double height_limit = std::min({
      max_size,
      std::max(host.bounds.height, static_cast<double>(kMinimumPanelSize)),
      static_cast<double>(work_area.height),
  });
  const double scale = std::min({
      1.0,
      width_limit / std::max(source_width, 1),
      height_limit / std::max(source_height, 1),
      max_size / std::max(source_width, source_height),
  });
  const int width = std::clamp(
      std::max(
          kMinimumPanelSize,
          static_cast<int>(std::floor(source_width * scale))),
      1,
      std::max(work_area.width, 1));
  const int height = std::clamp(
      std::max(
          kMinimumPanelSize,
          static_cast<int>(std::floor(source_height * scale))),
      1,
      std::max(work_area.height, 1));
  return {width, height};
}

PixbufPtr scale_pixbuf(GdkPixbuf* source, int width, int height) {
  const int source_width = gdk_pixbuf_get_width(source);
  const int source_height = gdk_pixbuf_get_height(source);
  const double scale = std::min(
      static_cast<double>(width) / source_width,
      static_cast<double>(height) / source_height);
  const int content_width = std::clamp(
      static_cast<int>(std::lround(source_width * scale)), 1, width);
  const int content_height = std::clamp(
      static_cast<int>(std::lround(source_height * scale)), 1, height);
  PixbufPtr result(gdk_pixbuf_new(
      GDK_COLORSPACE_RGB, TRUE, 8, width, height));
  if (!result) return {};
  gdk_pixbuf_fill(result.get(), 0x202124ff);
  const int x = (width - content_width) / 2;
  const int y = (height - content_height) / 2;
  gdk_pixbuf_composite(
      source,
      result.get(),
      x,
      y,
      content_width,
      content_height,
      x,
      y,
      scale,
      scale,
      GDK_INTERP_BILINEAR,
      255);
  return result;
}

void blend_pixel(
    GdkPixbuf* image,
    int x,
    int y,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue,
    std::uint8_t alpha) {
  if (x < 0 || y < 0 ||
      x >= gdk_pixbuf_get_width(image) ||
      y >= gdk_pixbuf_get_height(image)) {
    return;
  }
  const int channels = gdk_pixbuf_get_n_channels(image);
  guchar* pixel = gdk_pixbuf_get_pixels(image) +
      static_cast<std::size_t>(y) * gdk_pixbuf_get_rowstride(image) +
      static_cast<std::size_t>(x) * channels;
  const unsigned inverse = 255U - alpha;
  pixel[0] = static_cast<guchar>(
      (static_cast<unsigned>(red) * alpha + pixel[0] * inverse + 127U) /
      255U);
  pixel[1] = static_cast<guchar>(
      (static_cast<unsigned>(green) * alpha + pixel[1] * inverse + 127U) /
      255U);
  pixel[2] = static_cast<guchar>(
      (static_cast<unsigned>(blue) * alpha + pixel[2] * inverse + 127U) /
      255U);
  if (channels == 4) pixel[3] = 255;
}

void blend_rect(
    GdkPixbuf* image,
    RectI rect,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue,
    std::uint8_t alpha) {
  rect = intersect_rects(
      rect,
      {0, 0, gdk_pixbuf_get_width(image), gdk_pixbuf_get_height(image)});
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
      blend_pixel(image, x, y, red, green, blue, alpha);
    }
  }
}

void draw_controls(GdkPixbuf* image) {
  const int width = gdk_pixbuf_get_width(image);
  const int height = gdk_pixbuf_get_height(image);
  const ControlRects controls = control_rects(width, height);
  blend_rect(image, controls.hide, 24, 24, 24, 178);
  blend_rect(image, controls.relocate, 24, 24, 24, 178);

  const int hide_center_x = controls.hide.x + controls.hide.width / 2;
  const int hide_center_y = controls.hide.y + controls.hide.height / 2;
  const int hide_half = std::max(kControlStroke, controls.hide.width / 4);
  blend_rect(
      image,
      {hide_center_x - hide_half,
       hide_center_y,
       hide_half * 2 + 1,
       kControlStroke},
      255,
      255,
      255,
      230);

  const int move_center_x =
      controls.relocate.x + controls.relocate.width / 2;
  const int move_center_y =
      controls.relocate.y + controls.relocate.height / 2;
  const int move_half =
      std::max(kControlStroke, controls.relocate.width / 4);
  blend_rect(
      image,
      {move_center_x - move_half,
       move_center_y,
       move_half * 2 + 1,
       kControlStroke},
      255,
      255,
      255,
      230);
  blend_rect(
      image,
      {move_center_x,
       move_center_y - move_half,
       kControlStroke,
       move_half * 2 + 1},
      255,
      255,
      255,
      230);
}

void draw_icon(GdkPixbuf* image, GdkPixbuf* icon) {
  if (icon == nullptr) return;
  const int width = gdk_pixbuf_get_width(image);
  const int height = gdk_pixbuf_get_height(image);
  const int available = std::min(width, height) - kIconMargin * 2;
  const int size = std::min(kIconSize, available);
  if (size <= 0) return;
  PixbufPtr scaled(gdk_pixbuf_scale_simple(
      icon, size, size, GDK_INTERP_BILINEAR));
  if (!scaled) return;
  gdk_pixbuf_composite(
      scaled.get(),
      image,
      kIconMargin,
      height - kIconMargin - size,
      size,
      size,
      kIconMargin,
      height - kIconMargin - size,
      1,
      1,
      GDK_INTERP_NEAREST,
      255);
}

std::pair<int, int> presentation_origin(
    const OverlayHost& host,
    int width,
    int height,
    double cursor,
    const RectI& work_area) {
  const int offset = static_cast<int>(std::lround(host.anchor.offset));
  const int stack_offset = static_cast<int>(std::lround(cursor));
  switch (host.anchor.edge) {
    case AnchorEdge::kLeading:
      return {work_area.x + offset, work_area.y + offset + stack_offset};
    case AnchorEdge::kTrailing:
      return {
          work_area.x + work_area.width - offset - width,
          work_area.y + offset + stack_offset,
      };
    case AnchorEdge::kTop:
      return {work_area.x + offset + stack_offset, work_area.y + offset};
    case AnchorEdge::kBottom:
      return {
          work_area.x + offset + stack_offset,
          work_area.y + work_area.height - offset - height,
      };
  }
  return {work_area.x, work_area.y};
}

bool presentation_fits(
    const OverlayHost& host,
    int width,
    int height,
    double cursor,
    const RectI& work_area) {
  const double offset = host.anchor.offset;
  if (host.anchor.edge == AnchorEdge::kLeading ||
      host.anchor.edge == AnchorEdge::kTrailing) {
    return offset + width <= work_area.width &&
           offset + cursor + height <= work_area.height;
  }
  return offset + height <= work_area.height &&
         offset + cursor + width <= work_area.width;
}

std::pair<int, int> clamped_origin(
    int x,
    int y,
    int width,
    int height,
    const RectI& work_area) {
  const int maximum_x =
      std::max(work_area.x, work_area.x + work_area.width - width);
  const int maximum_y =
      std::max(work_area.y, work_area.y + work_area.height - height);
  return {
      std::clamp(x, work_area.x, maximum_x),
      std::clamp(y, work_area.y, maximum_y),
  };
}

xcb_atom_t intern_atom(xcb_connection_t* connection, const char* name) {
  const xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(connection, false, std::strlen(name), name);
  std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply(
      xcb_intern_atom_reply(connection, cookie, nullptr), &std::free);
  if (!reply) {
    throw std::runtime_error(
        std::string("X11 could not intern atom ") + name);
  }
  return reply->atom;
}

std::uint8_t mask_shift(std::uint32_t mask) {
  if (mask == 0) return 0;
  std::uint8_t result = 0;
  while ((mask & 1U) == 0) {
    mask >>= 1U;
    ++result;
  }
  return result;
}

std::uint32_t component_value(
    std::uint8_t component,
    std::uint32_t mask) {
  if (mask == 0) return 0;
  const std::uint8_t shift = mask_shift(mask);
  const std::uint32_t maximum = mask >> shift;
  return ((static_cast<std::uint32_t>(component) * maximum + 127U) / 255U)
      << shift;
}

std::string exception_message(std::exception_ptr exception) {
  if (!exception) return "Linux overlay worker failed";
  try {
    std::rethrow_exception(exception);
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "Linux overlay worker failed";
  }
}

class LinuxOverlayPlatform final : public OverlayPlatform {
 public:
  explicit LinuxOverlayPlatform(OverlayPlatformEvents events)
      : events_(std::move(events)) {
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
      throw std::runtime_error("eventfd failed for the Linux overlay worker");
    }
    worker_ = std::thread([this] { run(); });
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return startup_complete_; });
    if (startup_error_) {
      lock.unlock();
      worker_.join();
      close(wake_fd_);
      wake_fd_ = -1;
      std::rethrow_exception(startup_error_);
    }
  }

  ~LinuxOverlayPlatform() override { stop(); }

  void update(const OverlaySnapshot& snapshot) override {
    std::uint64_t generation = 0;
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || worker_error_) {
        throw std::runtime_error(exception_message(worker_error_));
      }
      pending_snapshot_ = snapshot;
      generation = ++requested_generation_;
    }
    wake_worker();

    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this, generation] {
      return applied_generation_ >= generation || stopping_ || worker_error_;
    });
    if (worker_error_) {
      throw std::runtime_error(exception_message(worker_error_));
    }
    if (failed_generation_ == generation) {
      throw std::runtime_error(last_update_error_);
    }
  }

  void stop() override {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        if (worker_.joinable() &&
            worker_.get_id() != std::this_thread::get_id()) {
          // Join outside the lock below.
        } else {
          return;
        }
      } else {
        stopping_ = true;
      }
    }
    wake_worker();
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
    }
    if (wake_fd_ >= 0) {
      close(wake_fd_);
      wake_fd_ = -1;
    }
  }

 private:
  void wake_worker() const {
    if (wake_fd_ < 0) return;
    const std::uint64_t value = 1;
    const ssize_t ignored = write(wake_fd_, &value, sizeof(value));
    (void)ignored;
  }

  void run() noexcept {
    try {
      initialize_x11();
      {
        std::lock_guard lock(mutex_);
        startup_complete_ = true;
      }
      condition_.notify_all();
      run_event_loop();
    } catch (...) {
      std::lock_guard lock(mutex_);
      if (!startup_complete_) {
        startup_error_ = std::current_exception();
        startup_complete_ = true;
      } else {
        worker_error_ = std::current_exception();
      }
      stopping_ = true;
      condition_.notify_all();
    }
    close_panels();
    if (connection_ != nullptr) {
      xcb_disconnect(connection_);
      connection_ = nullptr;
      screen_ = nullptr;
    }
  }

  void initialize_x11() {
    int screen_number = 0;
    connection_ = xcb_connect(nullptr, &screen_number);
    if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
      throw std::runtime_error(
          "nativekit overlays require an available X11/XWayland display; "
          "start Electron with --ozone-platform=x11");
    }

    const xcb_setup_t* setup = xcb_get_setup(connection_);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen_number && screens.rem > 0; ++index) {
      xcb_screen_next(&screens);
    }
    if (screens.rem == 0 || screens.data == nullptr) {
      throw std::runtime_error("X11 did not expose the requested screen");
    }
    screen_ = screens.data;
    initialize_pixel_format(setup);
    initialize_atoms();
    // xwayland-satellite re-presents each X11 window to the compositor as a
    // plain xdg-toplevel, which has no absolute-position request, so a
    // free-floating overlay cannot be moved on screen. A child of the Electron
    // host window is positioned relative to its parent, and that IS honored, so
    // under satellite we parent the panel to the host (clipped to host bounds).
    // Integrated XWayland and bare X11 honor top-level coordinates directly.
    embed_in_host_ = running_under_xwayland_satellite();
  }

  // xwayland-satellite advertises _NET_WM_NAME = "xwayland-satellite" on the
  // _NET_SUPPORTING_WM_CHECK window. Integrated XWayland reports the real
  // compositor's WM name there, and bare X11 has no check window.
  bool running_under_xwayland_satellite() const {
    std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> check(
        xcb_get_property_reply(
            connection_,
            xcb_get_property(
                connection_, false, screen_->root,
                intern_atom(connection_, "_NET_SUPPORTING_WM_CHECK"),
                XCB_ATOM_WINDOW, 0, 1),
            nullptr),
        &std::free);
    if (!check || check->format != 32 ||
        xcb_get_property_value_length(check.get()) < 4) {
      return false;
    }
    const xcb_window_t wm_window =
        *static_cast<const xcb_window_t*>(xcb_get_property_value(check.get()));
    if (wm_window == XCB_WINDOW_NONE) return false;
    std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> name(
        xcb_get_property_reply(
            connection_,
            xcb_get_property(
                connection_, false, wm_window, atoms_.net_wm_name,
                XCB_GET_PROPERTY_TYPE_ANY, 0, 64),
            nullptr),
        &std::free);
    if (!name || name->format != 8) return false;
    const int length = xcb_get_property_value_length(name.get());
    const auto* value =
        static_cast<const char*>(xcb_get_property_value(name.get()));
    return length > 0 && value != nullptr &&
        std::string(value, static_cast<std::size_t>(length)) ==
            "xwayland-satellite";
  }

  void initialize_pixel_format(const xcb_setup_t* setup) {
    pixel_format_.depth = screen_->root_depth;
    pixel_format_.image_byte_order = setup->image_byte_order;

    for (xcb_format_iterator_t iterator =
             xcb_setup_pixmap_formats_iterator(setup);
         iterator.rem > 0;
         xcb_format_next(&iterator)) {
      if (iterator.data->depth != screen_->root_depth) continue;
      pixel_format_.bits_per_pixel = iterator.data->bits_per_pixel;
      pixel_format_.scanline_pad = iterator.data->scanline_pad;
      break;
    }

    for (xcb_depth_iterator_t depths =
             xcb_screen_allowed_depths_iterator(screen_);
         depths.rem > 0;
         xcb_depth_next(&depths)) {
      if (depths.data->depth != screen_->root_depth) continue;
      for (xcb_visualtype_iterator_t visuals =
               xcb_depth_visuals_iterator(depths.data);
           visuals.rem > 0;
           xcb_visualtype_next(&visuals)) {
        if (visuals.data->visual_id != screen_->root_visual) continue;
        pixel_format_.red_mask = visuals.data->red_mask;
        pixel_format_.green_mask = visuals.data->green_mask;
        pixel_format_.blue_mask = visuals.data->blue_mask;
        break;
      }
    }

    if ((pixel_format_.bits_per_pixel != 16 &&
         pixel_format_.bits_per_pixel != 24 &&
         pixel_format_.bits_per_pixel != 32) ||
        pixel_format_.scanline_pad == 0 ||
        pixel_format_.red_mask == 0 ||
        pixel_format_.green_mask == 0 ||
        pixel_format_.blue_mask == 0) {
      throw std::runtime_error("X11 root visual is unsupported");
    }
  }

  void initialize_atoms() {
    atoms_.utf8_string = intern_atom(connection_, "UTF8_STRING");
    atoms_.net_wm_name = intern_atom(connection_, "_NET_WM_NAME");
    atoms_.net_wm_window_type =
        intern_atom(connection_, "_NET_WM_WINDOW_TYPE");
    atoms_.net_wm_window_type_utility =
        intern_atom(connection_, "_NET_WM_WINDOW_TYPE_UTILITY");
    atoms_.net_wm_state = intern_atom(connection_, "_NET_WM_STATE");
    atoms_.net_wm_state_above =
        intern_atom(connection_, "_NET_WM_STATE_ABOVE");
    atoms_.net_wm_state_skip_taskbar =
        intern_atom(connection_, "_NET_WM_STATE_SKIP_TASKBAR");
    atoms_.net_wm_state_skip_pager =
        intern_atom(connection_, "_NET_WM_STATE_SKIP_PAGER");
    atoms_.net_wm_state_sticky =
        intern_atom(connection_, "_NET_WM_STATE_STICKY");
    atoms_.net_wm_desktop = intern_atom(connection_, "_NET_WM_DESKTOP");
    atoms_.net_workarea = intern_atom(connection_, "_NET_WORKAREA");
    atoms_.net_current_desktop =
        intern_atom(connection_, "_NET_CURRENT_DESKTOP");
    atoms_.motif_wm_hints = intern_atom(connection_, "_MOTIF_WM_HINTS");
  }

  void run_event_loop() {
    const int xcb_fd = xcb_get_file_descriptor(connection_);
    if (xcb_fd < 0) throw std::runtime_error("X11 connection has no file descriptor");

    while (true) {
      pollfd descriptors[2] = {
          {wake_fd_, POLLIN, 0},
          {xcb_fd, POLLIN, 0},
      };
      const int result = poll(descriptors, 2, -1);
      if (result < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("poll failed for the Linux overlay worker");
      }
      if ((descriptors[0].revents & POLLIN) != 0) {
        drain_wake_fd();
        if (should_stop()) break;
        apply_pending_snapshot();
      }
      if ((descriptors[1].revents & POLLIN) != 0) process_x11_events();
      if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
          xcb_connection_has_error(connection_) != 0) {
        throw std::runtime_error("X11 overlay connection was lost");
      }
    }
  }

  void drain_wake_fd() const {
    std::uint64_t value = 0;
    while (read(wake_fd_, &value, sizeof(value)) == sizeof(value)) {
    }
  }

  bool should_stop() const {
    std::lock_guard lock(mutex_);
    return stopping_;
  }

  void apply_pending_snapshot() {
    OverlaySnapshot snapshot;
    std::uint64_t generation = 0;
    {
      std::lock_guard lock(mutex_);
      if (!pending_snapshot_ ||
          requested_generation_ <= applied_generation_) {
        return;
      }
      snapshot = *pending_snapshot_;
      generation = requested_generation_;
    }

    std::string error;
    try {
      apply_update(snapshot);
      if (xcb_flush(connection_) <= 0 &&
          xcb_connection_has_error(connection_) != 0) {
        throw std::runtime_error("X11 overlay update could not be flushed");
      }
    } catch (...) {
      error = exception_message(std::current_exception());
    }

    {
      std::lock_guard lock(mutex_);
      applied_generation_ = generation;
      if (error.empty()) {
        failed_generation_ = 0;
        last_update_error_.clear();
      } else {
        failed_generation_ = generation;
        last_update_error_ = std::move(error);
      }
    }
    condition_.notify_all();
  }

  void validate_host(const OverlayHost& host) const {
    if (host.window_handle == 0 ||
        host.window_handle > std::numeric_limits<xcb_window_t>::max()) {
      throw std::runtime_error(
          "nativekit overlays require an X11 BrowserWindow handle");
    }
    const xcb_window_t window =
        static_cast<xcb_window_t>(host.window_handle);
    xcb_generic_error_t* raw_error = nullptr;
    std::unique_ptr<xcb_get_window_attributes_reply_t, decltype(&std::free)>
        reply(
            xcb_get_window_attributes_reply(
                connection_,
                xcb_get_window_attributes(connection_, window),
                &raw_error),
            &std::free);
    std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error(
        raw_error, &std::free);
    if (!reply || error) {
      throw std::runtime_error(
          "nativekit overlays require Electron to use X11/XWayland; "
          "start Electron with --ozone-platform=x11");
    }
  }

  std::vector<std::uint32_t> cardinal_property(
      xcb_window_t window,
      xcb_atom_t property,
      std::uint32_t maximum_items = 64) const {
    std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)> reply(
        xcb_get_property_reply(
            connection_,
            xcb_get_property(
                connection_,
                false,
                window,
                property,
                XCB_ATOM_CARDINAL,
                0,
                maximum_items),
            nullptr),
        &std::free);
    if (!reply || reply->format != 32) return {};
    const int length = xcb_get_property_value_length(reply.get()) / 4;
    const auto* values = static_cast<const std::uint32_t*>(
        xcb_get_property_value(reply.get()));
    return values == nullptr || length <= 0
        ? std::vector<std::uint32_t>{}
        : std::vector<std::uint32_t>(values, values + length);
  }

  RectI desktop_work_area() const {
    const RectI fallback{
        0,
        0,
        static_cast<int>(screen_->width_in_pixels),
        static_cast<int>(screen_->height_in_pixels),
    };
    const auto areas = cardinal_property(screen_->root, atoms_.net_workarea);
    if (areas.size() < 4) return fallback;
    const auto current = cardinal_property(
        screen_->root, atoms_.net_current_desktop, 1);
    const std::size_t desktop = current.empty() ? 0 : current.front();
    const std::size_t count = areas.size() / 4;
    const std::size_t index = std::min(desktop, count - 1) * 4;
    RectI result{
        static_cast<std::int32_t>(areas[index]),
        static_cast<std::int32_t>(areas[index + 1]),
        static_cast<int>(areas[index + 2]),
        static_cast<int>(areas[index + 3]),
    };
    return result.width > 0 && result.height > 0 ? result : fallback;
  }

  RectI monitor_for_host(const OverlayHost& host) const {
    const int center_x = static_cast<int>(
        std::lround(host.bounds.x + host.bounds.width / 2));
    const int center_y = static_cast<int>(
        std::lround(host.bounds.y + host.bounds.height / 2));
    const RectI fallback{
        0,
        0,
        static_cast<int>(screen_->width_in_pixels),
        static_cast<int>(screen_->height_in_pixels),
    };
    std::unique_ptr<xcb_randr_get_monitors_reply_t, decltype(&std::free)> reply(
        xcb_randr_get_monitors_reply(
            connection_,
            xcb_randr_get_monitors(connection_, screen_->root, true),
            nullptr),
        &std::free);
    if (!reply) return fallback;

    std::optional<RectI> nearest;
    std::int64_t nearest_distance = std::numeric_limits<std::int64_t>::max();
    for (xcb_randr_monitor_info_iterator_t iterator =
             xcb_randr_get_monitors_monitors_iterator(reply.get());
         iterator.rem > 0;
         xcb_randr_monitor_info_next(&iterator)) {
      const RectI monitor{
          iterator.data->x,
          iterator.data->y,
          iterator.data->width,
          iterator.data->height,
      };
      if (monitor.width <= 0 || monitor.height <= 0) continue;
      if (contains(monitor, center_x, center_y)) return monitor;
      const int closest_x = std::clamp(
          center_x, monitor.x, monitor.x + monitor.width - 1);
      const int closest_y = std::clamp(
          center_y, monitor.y, monitor.y + monitor.height - 1);
      const std::int64_t delta_x = center_x - closest_x;
      const std::int64_t delta_y = center_y - closest_y;
      const std::int64_t distance =
          delta_x * delta_x + delta_y * delta_y;
      if (distance < nearest_distance) {
        nearest = monitor;
        nearest_distance = distance;
      }
    }
    return nearest.value_or(fallback);
  }

  RectI work_area_for_host(const OverlayHost& host) const {
    if (embed_in_host_ && host.window_handle != 0 &&
        host.window_handle <=
            static_cast<std::uint64_t>(
                std::numeric_limits<xcb_window_t>::max())) {
      // Embedded child: origin is the host top-left and the clamp is the host's
      // real pixel size, so anchoring, clamping, and drag all stay in the
      // parent's coordinate space.
      std::unique_ptr<xcb_get_geometry_reply_t, decltype(&std::free)> geometry(
          xcb_get_geometry_reply(
              connection_,
              xcb_get_geometry(
                  connection_,
                  static_cast<xcb_window_t>(host.window_handle)),
              nullptr),
          &std::free);
      if (geometry && geometry->width > 0 && geometry->height > 0) {
        return {0, 0, geometry->width, geometry->height};
      }
    }
    const RectI monitor = monitor_for_host(host);
    const RectI available = intersect_rects(monitor, desktop_work_area());
    return available.width > 0 && available.height > 0
        ? available
        : monitor;
  }

  std::vector<std::uint8_t> native_pixels(GdkPixbuf* image) const {
    const int width = gdk_pixbuf_get_width(image);
    const int height = gdk_pixbuf_get_height(image);
    const int channels = gdk_pixbuf_get_n_channels(image);
    const int bytes_per_pixel = pixel_format_.bits_per_pixel / 8;
    const std::size_t unpadded =
        static_cast<std::size_t>(width) * bytes_per_pixel;
    const std::size_t pad_bytes = pixel_format_.scanline_pad / 8;
    const std::size_t stride =
        ((unpadded + pad_bytes - 1) / pad_bytes) * pad_bytes;
    std::vector<std::uint8_t> result(stride * height, 0);
    const guchar* source = gdk_pixbuf_get_pixels(image);
    const int source_stride = gdk_pixbuf_get_rowstride(image);

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const guchar* input = source +
            static_cast<std::size_t>(y) * source_stride +
            static_cast<std::size_t>(x) * channels;
        const std::uint32_t pixel =
            component_value(input[0], pixel_format_.red_mask) |
            component_value(input[1], pixel_format_.green_mask) |
            component_value(input[2], pixel_format_.blue_mask);
        std::uint8_t* output = result.data() +
            static_cast<std::size_t>(y) * stride +
            static_cast<std::size_t>(x) * bytes_per_pixel;
        for (int byte = 0; byte < bytes_per_pixel; ++byte) {
          const int shift = pixel_format_.image_byte_order ==
                                    XCB_IMAGE_ORDER_LSB_FIRST
              ? byte * 8
              : (bytes_per_pixel - byte - 1) * 8;
          output[byte] = static_cast<std::uint8_t>(pixel >> shift);
        }
      }
    }
    return result;
  }

  void upload_background(PanelState& state, GdkPixbuf* image) {
    const int width = gdk_pixbuf_get_width(image);
    const int height = gdk_pixbuf_get_height(image);
    const int bytes_per_pixel = pixel_format_.bits_per_pixel / 8;
    const std::size_t unpadded =
        static_cast<std::size_t>(width) * bytes_per_pixel;
    const std::size_t pad_bytes = pixel_format_.scanline_pad / 8;
    const std::size_t stride =
        ((unpadded + pad_bytes - 1) / pad_bytes) * pad_bytes;
    const std::vector<std::uint8_t> pixels = native_pixels(image);

    const xcb_pixmap_t pixmap = xcb_generate_id(connection_);
    xcb_create_pixmap(
        connection_,
        pixel_format_.depth,
        pixmap,
        screen_->root,
        static_cast<std::uint16_t>(width),
        static_cast<std::uint16_t>(height));
    const xcb_gcontext_t graphics = xcb_generate_id(connection_);
    xcb_create_gc(connection_, graphics, pixmap, 0, nullptr);

    const std::size_t maximum_request =
        static_cast<std::size_t>(xcb_get_maximum_request_length(connection_)) *
        4;
    const std::size_t payload_limit =
        maximum_request > 128 ? maximum_request - 128 : stride;
    const int rows_per_request = std::max<int>(
        1, static_cast<int>(payload_limit / std::max<std::size_t>(stride, 1)));
    for (int y = 0; y < height; y += rows_per_request) {
      const int rows = std::min(rows_per_request, height - y);
      const std::size_t length = stride * rows;
      xcb_put_image(
          connection_,
          XCB_IMAGE_FORMAT_Z_PIXMAP,
          pixmap,
          graphics,
          static_cast<std::uint16_t>(width),
          static_cast<std::uint16_t>(rows),
          0,
          static_cast<std::int16_t>(y),
          0,
          pixel_format_.depth,
          static_cast<std::uint32_t>(length),
          pixels.data() + static_cast<std::size_t>(y) * stride);
    }
    xcb_free_gc(connection_, graphics);

    const std::uint32_t background = pixmap;
    xcb_change_window_attributes(
        connection_, state.window, XCB_CW_BACK_PIXMAP, &background);
    xcb_clear_area(connection_, false, state.window, 0, 0, 0, 0);
    if (state.background != XCB_PIXMAP_NONE) {
      xcb_free_pixmap(connection_, state.background);
    }
    state.background = pixmap;
  }

  void set_text_property(
      xcb_window_t window,
      xcb_atom_t property,
      xcb_atom_t type,
      const std::string& value) const {
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        window,
        property,
        type,
        8,
        static_cast<std::uint32_t>(value.size()),
        value.data());
  }

  void configure_window(const RenderItem& item, PanelState& state) {
    const std::uint32_t values[] = {
        static_cast<std::uint32_t>(item.x),
        static_cast<std::uint32_t>(item.y),
        static_cast<std::uint32_t>(item.width),
        static_cast<std::uint32_t>(item.height),
        XCB_STACK_MODE_ABOVE,
    };
    xcb_configure_window(
        connection_,
        state.window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
            XCB_CONFIG_WINDOW_STACK_MODE,
        values);
  }

  std::unique_ptr<PanelState> create_panel(const RenderItem& item) {
    auto state = std::make_unique<PanelState>();
    state->id = item.id;
    state->host_id = item.host_id;
    state->window = xcb_generate_id(connection_);
    // Parent to the Electron host under xwayland-satellite; the root otherwise.
    const xcb_window_t parent =
        embed_in_host_ && item.host_window != XCB_WINDOW_NONE
        ? item.host_window
        : screen_->root;
    const std::uint32_t values[] = {
        screen_->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY,
    };
    const xcb_void_cookie_t create_cookie = xcb_create_window_checked(
        connection_,
        pixel_format_.depth,
        state->window,
        parent,
        static_cast<std::int16_t>(item.x),
        static_cast<std::int16_t>(item.y),
        static_cast<std::uint16_t>(item.width),
        static_cast<std::uint16_t>(item.height),
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen_->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
        values);
    std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> create_error(
        xcb_request_check(connection_, create_cookie), &std::free);
    if (create_error) {
      throw std::runtime_error("X11 could not create an overlay window");
    }

    const xcb_atom_t type = atoms_.net_wm_window_type_utility;
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        state->window,
        atoms_.net_wm_window_type,
        XCB_ATOM_ATOM,
        32,
        1,
        &type);
    const xcb_atom_t states[] = {
        atoms_.net_wm_state_above,
        atoms_.net_wm_state_skip_taskbar,
        atoms_.net_wm_state_skip_pager,
        atoms_.net_wm_state_sticky,
    };
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        state->window,
        atoms_.net_wm_state,
        XCB_ATOM_ATOM,
        32,
        4,
        states);
    const std::uint32_t all_desktops = 0xffffffffU;
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        state->window,
        atoms_.net_wm_desktop,
        XCB_ATOM_CARDINAL,
        32,
        1,
        &all_desktops);
    const std::uint32_t motif_hints[] = {2, 0, 0, 0, 0};
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        state->window,
        atoms_.motif_wm_hints,
        atoms_.motif_wm_hints,
        32,
        5,
        motif_hints);
    const std::uint32_t wm_hints[] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        state->window,
        XCB_ATOM_WM_HINTS,
        XCB_ATOM_WM_HINTS,
        32,
        9,
        wm_hints);
    const char wm_class[] = "nativekit\0Nativekit\0";
    xcb_change_property(
        connection_,
        XCB_PROP_MODE_REPLACE,
        state->window,
        XCB_ATOM_WM_CLASS,
        XCB_ATOM_STRING,
        8,
        sizeof(wm_class) - 1,
        wm_class);
    return state;
  }

  void apply_item(const RenderItem& item) {
    auto existing = panels_.find(item.id);
    if (existing == panels_.end()) {
      existing = panels_.emplace(item.id, create_panel(item)).first;
    }
    PanelState& state = *existing->second;
    state.host_id = item.host_id;
    state.work_area = item.work_area;
    state.x = item.x;
    state.y = item.y;
    state.width = item.width;
    state.height = item.height;
    if (item.preserve_position) {
      state.manual_x = item.x;
      state.manual_y = item.y;
    }

    set_text_property(
        state.window,
        atoms_.net_wm_name,
        atoms_.utf8_string,
        item.title);
    set_text_property(
        state.window,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        item.title);
    if (item.host_window != XCB_WINDOW_NONE) {
      xcb_change_property(
          connection_,
          XCB_PROP_MODE_REPLACE,
          state.window,
          XCB_ATOM_WM_TRANSIENT_FOR,
          XCB_ATOM_WINDOW,
          32,
          1,
          &item.host_window);
    }
    configure_window(item, state);
    upload_background(state, item.image.get());

    if (item.visible && !state.visible) {
      xcb_map_window(connection_, state.window);
    } else if (!item.visible && state.visible) {
      xcb_unmap_window(connection_, state.window);
    }
    state.visible = item.visible;
  }

  void destroy_panel(PanelState& state) noexcept {
    if (state.window != XCB_WINDOW_NONE && connection_ != nullptr) {
      xcb_destroy_window(connection_, state.window);
      state.window = XCB_WINDOW_NONE;
    }
    if (state.background != XCB_PIXMAP_NONE && connection_ != nullptr) {
      xcb_free_pixmap(connection_, state.background);
      state.background = XCB_PIXMAP_NONE;
    }
  }

  void close_panels() noexcept {
    if (connection_ == nullptr) return;
    for (auto& [id, panel] : panels_) destroy_panel(*panel);
    panels_.clear();
    xcb_flush(connection_);
  }

  void apply_update(const OverlaySnapshot& snapshot) {
    std::unordered_map<std::string, const OverlayHost*> hosts;
    hosts.reserve(snapshot.hosts.size());
    for (const auto& host : snapshot.hosts) {
      validate_host(host);
      hosts.emplace(host.id, &host);
    }

    std::unordered_map<std::string, HostLayout> layouts;
    std::unordered_set<std::string> active_ids;
    std::vector<RenderItem> items;
    items.reserve(snapshot.presentations.size());
    for (const auto& presentation : snapshot.presentations) {
      const auto host = hosts.find(presentation.host_id);
      if (host == hosts.end()) continue;
      active_ids.insert(presentation.id);

      auto layout = layouts.find(presentation.host_id);
      if (layout == layouts.end()) {
        layout = layouts
                     .emplace(
                         presentation.host_id,
                         HostLayout{work_area_for_host(*host->second), 0})
                     .first;
      }

      PixbufPtr source(pixbuf_from_data_url(presentation.image_data));
      const auto [width, height] = fitted_size(
          source.get(),
          *host->second,
          snapshot.max_size,
          layout->second.work_area);
      RenderItem item;
      item.id = presentation.id;
      item.host_id = presentation.host_id;
      item.title = host->second->title;
      item.host_window =
          static_cast<xcb_window_t>(host->second->window_handle);
      item.image = scale_pixbuf(source.get(), width, height);
      if (!item.image) {
        throw std::runtime_error("GdkPixbuf could not scale the overlay image");
      }
      if (presentation.app_icon_path) {
        PixbufPtr icon(app_icon_pixbuf(
            *presentation.app_icon_path, kIconSize));
        draw_icon(item.image.get(), icon.get());
      }
      draw_controls(item.image.get());
      item.work_area = layout->second.work_area;
      item.width = width;
      item.height = height;

      const bool eligible = presentation.visible && snapshot.visible;
      const bool stack_slot_fits = presentation_fits(
          *host->second,
          width,
          height,
          layout->second.cursor,
          layout->second.work_area);
      const auto existing = panels_.find(presentation.id);
      item.preserve_position =
          existing != panels_.end() &&
          existing->second->manually_positioned;
      if (item.preserve_position) {
        std::tie(item.x, item.y) = clamped_origin(
            existing->second->manual_x,
            existing->second->manual_y,
            width,
            height,
            item.work_area);
        item.visible = eligible;
      } else {
        std::tie(item.x, item.y) = presentation_origin(
            *host->second,
            width,
            height,
            layout->second.cursor,
            layout->second.work_area);
        item.visible = eligible && stack_slot_fits;
      }
      if (eligible && (stack_slot_fits || item.preserve_position)) {
        layout->second.cursor +=
            (host->second->anchor.edge == AnchorEdge::kLeading ||
             host->second->anchor.edge == AnchorEdge::kTrailing)
                ? height + kStackGap
                : width + kStackGap;
      }
      items.push_back(std::move(item));
    }

    for (auto& item : items) apply_item(item);
    for (auto iterator = panels_.begin(); iterator != panels_.end();) {
      if (active_ids.find(iterator->first) != active_ids.end()) {
        ++iterator;
        continue;
      }
      destroy_panel(*iterator->second);
      iterator = panels_.erase(iterator);
    }
  }

  PanelState* panel_for_window(xcb_window_t window) const {
    for (const auto& [id, panel] : panels_) {
      if (panel->window == window) return panel.get();
    }
    return nullptr;
  }

  void process_x11_events() {
    while (xcb_generic_event_t* event = xcb_poll_for_event(connection_)) {
      const std::uint8_t type = event->response_type & ~0x80U;
      switch (type) {
        case XCB_EXPOSE:
          handle_expose(*reinterpret_cast<xcb_expose_event_t*>(event));
          break;
        case XCB_BUTTON_PRESS:
          handle_button_press(
              *reinterpret_cast<xcb_button_press_event_t*>(event));
          break;
        case XCB_BUTTON_RELEASE:
          handle_button_release(
              *reinterpret_cast<xcb_button_release_event_t*>(event));
          break;
        case XCB_MOTION_NOTIFY:
          handle_motion(*reinterpret_cast<xcb_motion_notify_event_t*>(event));
          break;
        case XCB_CONFIGURE_NOTIFY:
          handle_configure(
              *reinterpret_cast<xcb_configure_notify_event_t*>(event));
          break;
        case XCB_DESTROY_NOTIFY:
          handle_destroy(*reinterpret_cast<xcb_destroy_notify_event_t*>(event));
          break;
        default:
          break;
      }
      std::free(event);
    }
    xcb_flush(connection_);
  }

  void handle_expose(const xcb_expose_event_t& event) const {
    if (panel_for_window(event.window) == nullptr) return;
    xcb_clear_area(
        connection_, false, event.window, event.x, event.y,
        event.width, event.height);
  }

  void handle_button_press(const xcb_button_press_event_t& event) {
    if (event.detail != XCB_BUTTON_INDEX_1) return;
    PanelState* state = panel_for_window(event.event);
    if (state == nullptr) return;
    const Control control = hit_test_control(*state, event.event_x, event.event_y);
    if (control != Control::kNone) {
      state->pressed_control = control;
      return;
    }
    const std::uint32_t elapsed = event.time - state->last_click_time;
    if (state->last_click_time != 0 &&
        elapsed <= kDoubleClickMilliseconds) {
      state->last_click_time = 0;
      if (events_.activate) events_.activate();
      return;
    }
    state->last_click_time = event.time;
    state->dragging = true;
    state->drag_moved = false;
    state->drag_start_root_x = event.root_x;
    state->drag_start_root_y = event.root_y;
    state->drag_start_x = state->x;
    state->drag_start_y = state->y;
  }

  void handle_motion(const xcb_motion_notify_event_t& event) {
    PanelState* state = panel_for_window(event.event);
    if (state == nullptr || !state->dragging ||
        (event.state & XCB_BUTTON_MASK_1) == 0) {
      return;
    }
    const int x = state->drag_start_x + event.root_x - state->drag_start_root_x;
    const int y = state->drag_start_y + event.root_y - state->drag_start_root_y;
    const auto [clamped_x, clamped_y] = clamped_origin(
        x, y, state->width, state->height, state->work_area);
    if (clamped_x == state->x && clamped_y == state->y) return;
    state->x = clamped_x;
    state->y = clamped_y;
    state->drag_moved = true;
    const std::uint32_t values[] = {
        static_cast<std::uint32_t>(state->x),
        static_cast<std::uint32_t>(state->y),
        XCB_STACK_MODE_ABOVE,
    };
    xcb_configure_window(
        connection_,
        state->window,
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_STACK_MODE,
        values);
  }

  void handle_button_release(const xcb_button_release_event_t& event) {
    if (event.detail != XCB_BUTTON_INDEX_1) return;
    PanelState* state = panel_for_window(event.event);
    if (state == nullptr) return;
    if (state->dragging) {
      state->dragging = false;
      if (state->drag_moved) {
        state->manually_positioned = true;
        state->manual_x = state->x;
        state->manual_y = state->y;
        state->drag_moved = false;
        if (events_.refresh) events_.refresh();
      }
      return;
    }

    const Control released =
        hit_test_control(*state, event.event_x, event.event_y);
    const Control pressed = std::exchange(
        state->pressed_control, Control::kNone);
    if (pressed == Control::kNone || pressed != released) return;
    if (pressed == Control::kHide && events_.visibility_request) {
      events_.visibility_request(false);
    } else if (pressed == Control::kRelocate && events_.relocate) {
      state->manually_positioned = false;
      events_.relocate(state->host_id);
    }
  }

  void handle_configure(const xcb_configure_notify_event_t& event) {
    PanelState* state = panel_for_window(event.window);
    if (state == nullptr) return;
    state->x = event.x;
    state->y = event.y;
  }

  void handle_destroy(const xcb_destroy_notify_event_t& event) {
    PanelState* state = panel_for_window(event.window);
    if (state == nullptr) return;
    state->window = XCB_WINDOW_NONE;
    state->visible = false;
  }

  OverlayPlatformEvents events_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::optional<OverlaySnapshot> pending_snapshot_;
  std::uint64_t requested_generation_ = 0;
  std::uint64_t applied_generation_ = 0;
  std::uint64_t failed_generation_ = 0;
  std::string last_update_error_;
  std::exception_ptr startup_error_;
  std::exception_ptr worker_error_;
  bool startup_complete_ = false;
  bool stopping_ = false;
  bool embed_in_host_ = false;
  int wake_fd_ = -1;

  xcb_connection_t* connection_ = nullptr;
  xcb_screen_t* screen_ = nullptr;
  PixelFormat pixel_format_;
  Atoms atoms_;
  std::unordered_map<std::string, std::unique_ptr<PanelState>> panels_;
};

}  // namespace

std::unique_ptr<OverlayPlatform> create_overlay_platform(
    OverlayPlatformEvents events) {
  return std::make_unique<LinuxOverlayPlatform>(std::move(events));
}

}  // namespace nativekit::platform
