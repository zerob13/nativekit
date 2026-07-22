#include "overlay/overlay_manager.h"

#include "common/win/image_utils.h"

#include <windows.h>

#include <CommCtrl.h>
#include <Wincrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nativekit::platform {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kDispatcherClassName[] =
    L"NativekitOverlayDispatcherWindow";
constexpr wchar_t kOverlayClassName[] = L"NativekitOverlayWindow";
constexpr UINT kInvokeMessage = WM_APP + 0x51;
constexpr std::size_t kMaximumDataUrlLength = 32 * 1024 * 1024;
constexpr std::uint64_t kMaximumDecodedBytes = 64 * 1024 * 1024;
constexpr UINT kMaximumImageDimension = 8192;
constexpr int kStackGap = 12;
constexpr int kControlMargin = 6;
constexpr int kControlGap = 4;
constexpr int kControlSize = 24;
constexpr int kControlStroke = 2;
constexpr int kIconMargin = 8;
constexpr int kIconSize = 28;
constexpr UINT_PTR kHideTooltipId = 1;
constexpr UINT_PTR kRelocateTooltipId = 2;

[[noreturn]] void throw_windows_error(const char* operation) {
  throw std::runtime_error(
      std::string(operation) + " failed (Windows error " +
      std::to_string(GetLastError()) + ")");
}

void require_hresult(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    throw std::runtime_error(
        std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<unsigned long>(result)) + ")");
  }
}

std::wstring wide_string(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (size <= 0) throw_windows_error("MultiByteToWideChar");
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          size) != size) {
    throw_windows_error("MultiByteToWideChar");
  }
  return result;
}

class UniqueBitmap {
 public:
  UniqueBitmap() = default;
  explicit UniqueBitmap(HBITMAP bitmap) : bitmap_(bitmap) {}
  UniqueBitmap(const UniqueBitmap&) = delete;
  UniqueBitmap& operator=(const UniqueBitmap&) = delete;
  UniqueBitmap(UniqueBitmap&& other) noexcept
      : bitmap_(std::exchange(other.bitmap_, nullptr)) {}
  UniqueBitmap& operator=(UniqueBitmap&& other) noexcept {
    if (this == &other) return *this;
    reset(std::exchange(other.bitmap_, nullptr));
    return *this;
  }
  ~UniqueBitmap() { reset(); }

  [[nodiscard]] HBITMAP get() const { return bitmap_; }

 private:
  void reset(HBITMAP bitmap = nullptr) {
    if (bitmap_ != nullptr) DeleteObject(bitmap_);
    bitmap_ = bitmap;
  }

  HBITMAP bitmap_ = nullptr;
};

enum class Control { kNone, kHide, kRelocate };

struct ControlRects {
  RECT hide{};
  RECT relocate{};
};

int scaled_pixels(int value, double scale_factor) {
  return std::max(1, static_cast<int>(std::lround(value * scale_factor)));
}

ControlRects control_rects(int width, int height, double scale_factor) {
  const int margin = std::max(
      1,
      std::min({
          scaled_pixels(kControlMargin, scale_factor),
          width / 8,
          height / 8,
      }));
  const int gap = std::max(
      1,
      std::min(scaled_pixels(kControlGap, scale_factor), width / 16));
  const int available_width = std::max(2, width - margin * 2 - gap);
  const int button = std::max(
      1,
      std::min({
          scaled_pixels(kControlSize, scale_factor),
          height - margin * 2,
          available_width / 2,
      }));
  ControlRects result;
  result.hide = {
      width - margin - button,
      margin,
      width - margin,
      margin + button,
  };
  result.relocate = {
      result.hide.left - gap - button,
      margin,
      result.hide.left - gap,
      margin + button,
  };
  return result;
}

void blend_pixel(
    BYTE* pixel,
    BYTE red,
    BYTE green,
    BYTE blue,
    BYTE alpha) {
  const unsigned inverse = 255U - alpha;
  pixel[0] = static_cast<BYTE>(
      (static_cast<unsigned>(blue) * alpha + pixel[0] * inverse + 127U) /
      255U);
  pixel[1] = static_cast<BYTE>(
      (static_cast<unsigned>(green) * alpha + pixel[1] * inverse + 127U) /
      255U);
  pixel[2] = static_cast<BYTE>(
      (static_cast<unsigned>(red) * alpha + pixel[2] * inverse + 127U) /
      255U);
  pixel[3] = static_cast<BYTE>(
      alpha + (static_cast<unsigned>(pixel[3]) * inverse + 127U) / 255U);
}

void blend_rect(
    BYTE* pixels,
    int width,
    int height,
    RECT rect,
    BYTE red,
    BYTE green,
    BYTE blue,
    BYTE alpha) {
  rect.left = std::clamp(rect.left, 0L, static_cast<LONG>(width));
  rect.right = std::clamp(rect.right, 0L, static_cast<LONG>(width));
  rect.top = std::clamp(rect.top, 0L, static_cast<LONG>(height));
  rect.bottom = std::clamp(rect.bottom, 0L, static_cast<LONG>(height));
  for (LONG y = rect.top; y < rect.bottom; ++y) {
    for (LONG x = rect.left; x < rect.right; ++x) {
      blend_pixel(
          pixels + (static_cast<std::size_t>(y) * width + x) * 4,
          red,
          green,
          blue,
          alpha);
    }
  }
}

void draw_controls(
    BYTE* pixels,
    int width,
    int height,
    double scale_factor) {
  const ControlRects rects = control_rects(width, height, scale_factor);
  blend_rect(pixels, width, height, rects.hide, 24, 24, 24, 178);
  blend_rect(pixels, width, height, rects.relocate, 24, 24, 24, 178);

  const LONG stroke = scaled_pixels(kControlStroke, scale_factor);
  const LONG hide_center_x = (rects.hide.left + rects.hide.right) / 2;
  const LONG hide_center_y = (rects.hide.top + rects.hide.bottom) / 2;
  const LONG hide_half = std::max<LONG>(
      stroke, (rects.hide.right - rects.hide.left) / 4);
  blend_rect(
      pixels,
      width,
      height,
      {hide_center_x - hide_half,
       hide_center_y,
       hide_center_x + hide_half + 1,
       hide_center_y + stroke},
      255,
      255,
      255,
      230);

  const LONG move_center_x =
      (rects.relocate.left + rects.relocate.right) / 2;
  const LONG move_center_y =
      (rects.relocate.top + rects.relocate.bottom) / 2;
  const LONG move_half = std::max<LONG>(
      stroke, (rects.relocate.right - rects.relocate.left) / 4);
  blend_rect(
      pixels,
      width,
      height,
      {move_center_x - move_half,
       move_center_y,
       move_center_x + move_half + 1,
       move_center_y + stroke},
      255,
      255,
      255,
      230);
  blend_rect(
      pixels,
      width,
      height,
      {move_center_x,
       move_center_y - move_half,
       move_center_x + stroke,
       move_center_y + move_half + 1},
      255,
      255,
      255,
      230);
}

void blend_pbgra_image(
    BYTE* destination,
    int destination_width,
    const BYTE* source,
    int source_width,
    int source_height,
    int destination_x,
    int destination_y) {
  for (int y = 0; y < source_height; ++y) {
    for (int x = 0; x < source_width; ++x) {
      BYTE* output = destination +
          (static_cast<std::size_t>(destination_y + y) * destination_width +
           destination_x + x) *
              4;
      const BYTE* input = source +
          (static_cast<std::size_t>(y) * source_width + x) * 4;
      const unsigned inverse = 255U - input[3];
      for (int channel = 0; channel < 3; ++channel) {
        output[channel] = static_cast<BYTE>(
            input[channel] +
            (static_cast<unsigned>(output[channel]) * inverse + 127U) / 255U);
      }
      output[3] = static_cast<BYTE>(
          input[3] +
          (static_cast<unsigned>(output[3]) * inverse + 127U) / 255U);
    }
  }
}

struct WindowState {
  HWND window = nullptr;
  HWND tooltip_window = nullptr;
  OverlayPlatformEvents* events = nullptr;
  std::string host_id;
  ControlRects controls;
  std::wstring hide_tooltip;
  std::wstring relocate_tooltip;
  Control pressed_control = Control::kNone;
};

TOOLINFOW tooltip_info(
    const WindowState& state,
    UINT_PTR id,
    const RECT& rect,
    wchar_t* text) {
  TOOLINFOW tool{};
  tool.cbSize = sizeof(tool);
  tool.uFlags = TTF_SUBCLASS;
  tool.hwnd = state.window;
  tool.uId = id;
  tool.rect = rect;
  tool.lpszText = text;
  return tool;
}

void update_tooltips(
    WindowState& state,
    const std::wstring& hide_text,
    const std::wstring& relocate_text) {
  state.hide_tooltip = hide_text;
  state.relocate_tooltip = relocate_text;
  TOOLINFOW hide = tooltip_info(
      state,
      kHideTooltipId,
      state.controls.hide,
      state.hide_tooltip.data());
  TOOLINFOW relocate = tooltip_info(
      state,
      kRelocateTooltipId,
      state.controls.relocate,
      state.relocate_tooltip.data());
  SendMessageW(
      state.tooltip_window,
      TTM_NEWTOOLRECTW,
      0,
      reinterpret_cast<LPARAM>(&hide));
  SendMessageW(
      state.tooltip_window,
      TTM_UPDATETIPTEXTW,
      0,
      reinterpret_cast<LPARAM>(&hide));
  SendMessageW(
      state.tooltip_window,
      TTM_NEWTOOLRECTW,
      0,
      reinterpret_cast<LPARAM>(&relocate));
  SendMessageW(
      state.tooltip_window,
      TTM_UPDATETIPTEXTW,
      0,
      reinterpret_cast<LPARAM>(&relocate));
}

Control hit_test_control(const WindowState& state, POINT point) {
  if (PtInRect(&state.controls.hide, point)) return Control::kHide;
  if (PtInRect(&state.controls.relocate, point)) return Control::kRelocate;
  return Control::kNone;
}

LRESULT CALLBACK overlay_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
  auto* state = reinterpret_cast<WindowState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    state = static_cast<WindowState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(
        window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }

  if (state != nullptr) {
    switch (message) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_LBUTTONDOWN: {
        const POINT point{
            static_cast<short>(LOWORD(lparam)),
            static_cast<short>(HIWORD(lparam)),
        };
        state->pressed_control = hit_test_control(*state, point);
        if (state->pressed_control != Control::kNone) SetCapture(window);
        return 0;
      }
      case WM_LBUTTONUP: {
        const POINT point{
            static_cast<short>(LOWORD(lparam)),
            static_cast<short>(HIWORD(lparam)),
        };
        const Control released_control = hit_test_control(*state, point);
        const Control pressed_control = state->pressed_control;
        state->pressed_control = Control::kNone;
        if (GetCapture() == window) ReleaseCapture();
        if (pressed_control != released_control || state->events == nullptr) {
          return 0;
        }
        if (released_control == Control::kHide &&
            state->events->visibility_request) {
          state->events->visibility_request(false);
        } else if (
            released_control == Control::kRelocate &&
            state->events->relocate) {
          state->events->relocate(state->host_id);
        }
        return 0;
      }
      case WM_LBUTTONDBLCLK: {
        const POINT point{
            static_cast<short>(LOWORD(lparam)),
            static_cast<short>(HIWORD(lparam)),
        };
        if (hit_test_control(*state, point) == Control::kNone &&
            state->events != nullptr && state->events->activate) {
          state->events->activate();
        }
        return 0;
      }
      case WM_CAPTURECHANGED:
        state->pressed_control = Control::kNone;
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
      }
      case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        state->window = nullptr;
        break;
      default:
        break;
    }
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

struct SyncInvocation {
  std::function<void()> callback;
  std::exception_ptr exception;
};

LRESULT CALLBACK dispatcher_window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
  if (message == kInvokeMessage) {
    auto* invocation = reinterpret_cast<SyncInvocation*>(lparam);
    try {
      invocation->callback();
    } catch (...) {
      invocation->exception = std::current_exception();
    }
    return 1;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void register_window_class(
    HINSTANCE instance,
    const wchar_t* name,
    WNDPROC procedure,
    UINT style) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = style;
  window_class.lpfnWndProc = procedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = name;
  if (RegisterClassExW(&window_class) != 0) return;
  if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    throw_windows_error("RegisterClassExW");
  }

  WNDCLASSEXW existing{};
  existing.cbSize = sizeof(existing);
  if (!GetClassInfoExW(instance, name, &existing) ||
      existing.lpfnWndProc != procedure) {
    throw std::runtime_error("overlay window class is already in use");
  }
}

std::vector<BYTE> decode_data_url(const std::string& data_url) {
  if (data_url.empty() || data_url.size() > kMaximumDataUrlLength) {
    throw std::runtime_error("overlay image data exceeds the 32 MiB limit");
  }
  const std::size_t comma = data_url.find(',');
  if (comma == std::string::npos || comma + 1 >= data_url.size()) {
    throw std::runtime_error("overlay image data URL is invalid");
  }
  std::string header = data_url.substr(0, comma);
  std::transform(
      header.begin(),
      header.end(),
      header.begin(),
      [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (header != "data:image/png;base64" &&
      header != "data:image/jpeg;base64" &&
      header != "data:image/jpg;base64") {
    throw std::runtime_error("overlay image must be a PNG or JPEG data URL");
  }

  const char* encoded = data_url.data() + comma + 1;
  const DWORD encoded_size = static_cast<DWORD>(data_url.size() - comma - 1);
  DWORD decoded_size = 0;
  if (!CryptStringToBinaryA(
          encoded,
          encoded_size,
          CRYPT_STRING_BASE64,
          nullptr,
          &decoded_size,
          nullptr,
          nullptr) ||
      decoded_size == 0) {
    throw std::runtime_error("overlay image base64 data is invalid");
  }
  std::vector<BYTE> decoded(decoded_size);
  if (!CryptStringToBinaryA(
          encoded,
          encoded_size,
          CRYPT_STRING_BASE64,
          decoded.data(),
          &decoded_size,
          nullptr,
          nullptr)) {
    throw std::runtime_error("overlay image base64 data is invalid");
  }
  decoded.resize(decoded_size);
  return decoded;
}

RECT work_area_for_host(const OverlayHost& host) {
  const HWND host_window = reinterpret_cast<HWND>(host.window_handle);
  HMONITOR monitor = nullptr;
  if (IsWindow(host_window)) {
    monitor = MonitorFromWindow(host_window, MONITOR_DEFAULTTONEAREST);
  }
  if (monitor == nullptr) {
    monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
  }
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
    throw_windows_error("GetMonitorInfoW");
  }
  return info.rcWork;
}

double scale_for_host(const OverlayHost& host) {
  const HWND host_window = reinterpret_cast<HWND>(host.window_handle);
  const UINT dpi = IsWindow(host_window) ? GetDpiForWindow(host_window) : 96;
  return (dpi == 0 ? 96U : dpi) / 96.0;
}

SIZE fitted_size(
    UINT image_width,
    UINT image_height,
    const OverlayHost& host,
    double max_size,
    double scale_factor,
    const RECT& work_area) {
  const int work_width = std::max<LONG>(1, work_area.right - work_area.left);
  const int work_height = std::max<LONG>(1, work_area.bottom - work_area.top);
  const double maximum_pixels = max_size * scale_factor;
  const double minimum_pixels = 64 * scale_factor;
  const double width_limit = std::min({
      maximum_pixels,
      std::max(host.bounds.width * scale_factor, minimum_pixels),
      static_cast<double>(work_width),
  });
  const double height_limit = std::min({
      maximum_pixels,
      std::max(host.bounds.height * scale_factor, minimum_pixels),
      static_cast<double>(work_height),
  });
  const double scale = std::min({
      1.0,
      width_limit / std::max<double>(image_width, 1),
      height_limit / std::max<double>(image_height, 1),
      maximum_pixels /
          std::max<double>(std::max(image_width, image_height), 1),
  });
  const int width = std::min(
      work_width,
      std::max(1, static_cast<int>(std::floor(image_width * scale))));
  const int height = std::min(
      work_height,
      std::max(1, static_cast<int>(std::floor(image_height * scale))));
  return {width, height};
}

LONG clamped_coordinate(double value, LONG minimum, LONG maximum) {
  if (maximum < minimum) maximum = minimum;
  if (!std::isfinite(value)) return value > 0 ? maximum : minimum;
  value = std::clamp(
      value, static_cast<double>(minimum), static_cast<double>(maximum));
  return static_cast<LONG>(std::lround(value));
}

RECT presentation_frame(
    const OverlayHost& host,
    SIZE size,
    double cursor,
    double scale_factor,
    const RECT& work_area) {
  const LONG maximum_x = std::max(work_area.left, work_area.right - size.cx);
  const LONG maximum_y = std::max(work_area.top, work_area.bottom - size.cy);
  const double offset = host.anchor.offset * scale_factor;
  double x = work_area.left;
  double y = work_area.top;
  switch (host.anchor.edge) {
    case AnchorEdge::kLeading:
      x = work_area.left + offset;
      y = work_area.top + offset + cursor;
      break;
    case AnchorEdge::kTrailing:
      x = work_area.right - offset - size.cx;
      y = work_area.top + offset + cursor;
      break;
    case AnchorEdge::kTop:
      x = work_area.left + offset + cursor;
      y = work_area.top + offset;
      break;
    case AnchorEdge::kBottom:
      x = work_area.left + offset + cursor;
      y = work_area.bottom - offset - size.cy;
      break;
  }
  const LONG left =
      clamped_coordinate(x, work_area.left, maximum_x);
  const LONG top = clamped_coordinate(y, work_area.top, maximum_y);
  return {left, top, left + size.cx, top + size.cy};
}

bool presentation_fits(
    const OverlayHost& host,
    SIZE size,
    double cursor,
    double scale_factor,
    const RECT& work_area) {
  const double width = work_area.right - work_area.left;
  const double height = work_area.bottom - work_area.top;
  const double offset = host.anchor.offset * scale_factor;
  if (host.anchor.edge == AnchorEdge::kLeading ||
      host.anchor.edge == AnchorEdge::kTrailing) {
    return offset + size.cx <= width &&
           offset + cursor + size.cy <= height;
  }
  return offset + size.cy <= height &&
         offset + cursor + size.cx <= width;
}

struct DecodedBitmap {
  UniqueBitmap bitmap;
  int width = 0;
  int height = 0;
};

struct RenderItem {
  std::string id;
  std::string host_id;
  std::wstring title;
  std::wstring hide_tooltip;
  std::wstring relocate_tooltip;
  DecodedBitmap image;
  RECT frame{};
  double scale_factor = 1;
  bool visible = false;
};

class WindowsOverlayPlatform final : public OverlayPlatform {
 public:
  explicit WindowsOverlayPlatform(OverlayPlatformEvents events)
      : events_(std::move(events)) {
    message_thread_ = std::thread([this] { run_message_thread(); });
    std::unique_lock lock(startup_mutex_);
    startup_condition_.wait(lock, [this] { return startup_complete_; });
    const std::exception_ptr startup_error = startup_error_;
    lock.unlock();
    if (startup_error != nullptr) {
      message_thread_.join();
      std::rethrow_exception(startup_error);
    }
  }

  ~WindowsOverlayPlatform() override {
    try {
      stop();
    } catch (...) {
    }
  }

  void update(const OverlaySnapshot& snapshot) override {
    if (stopped_.load(std::memory_order_acquire)) {
      throw std::runtime_error("overlay platform has stopped");
    }
    OverlaySnapshot copied_snapshot = snapshot;
    invoke_sync([this, snapshot = std::move(copied_snapshot)] {
      apply_update(snapshot);
    });
  }

  void stop() override {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) return;
    if (!message_thread_.joinable()) return;
    if (GetCurrentThreadId() == message_thread_id_.load()) {
      throw std::runtime_error(
          "overlay platform cannot stop from its message thread");
    }

    std::exception_ptr shutdown_error;
    try {
      invoke_sync([this] {
        close_all();
        PostQuitMessage(0);
      });
    } catch (...) {
      shutdown_error = std::current_exception();
      PostThreadMessageW(message_thread_id_.load(), WM_QUIT, 0, 0);
    }
    message_thread_.join();
    if (shutdown_error != nullptr) std::rethrow_exception(shutdown_error);
  }

 private:
  struct HostLayout {
    RECT work_area{};
    double cursor = 0;
    double scale_factor = 1;
  };

  void signal_startup(std::exception_ptr error = nullptr) {
    {
      std::lock_guard lock(startup_mutex_);
      startup_error_ = error;
      startup_complete_ = true;
    }
    startup_condition_.notify_one();
  }

  void run_message_thread() noexcept {
    message_thread_id_.store(GetCurrentThreadId(), std::memory_order_release);
    bool com_initialized = false;
    try {
      const HRESULT com_result =
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        require_hresult(com_result, "CoInitializeEx");
      }
      com_initialized = SUCCEEDED(com_result);

      instance_ = GetModuleHandleW(nullptr);
      if (instance_ == nullptr) throw_windows_error("GetModuleHandleW");
      INITCOMMONCONTROLSEX common_controls{};
      common_controls.dwSize = sizeof(common_controls);
      common_controls.dwICC = ICC_WIN95_CLASSES;
      if (!InitCommonControlsEx(&common_controls)) {
        throw_windows_error("InitCommonControlsEx");
      }
      register_window_class(
          instance_, kDispatcherClassName, dispatcher_window_proc, 0);
      register_window_class(
          instance_, kOverlayClassName, overlay_window_proc, CS_DBLCLKS);
      require_hresult(
          CoCreateInstance(
              CLSID_WICImagingFactory,
              nullptr,
              CLSCTX_INPROC_SERVER,
              IID_PPV_ARGS(&wic_factory_)),
          "CoCreateInstance(WICImagingFactory)");

      const HWND dispatcher = CreateWindowExW(
          0,
          kDispatcherClassName,
          L"",
          0,
          0,
          0,
          0,
          0,
          HWND_MESSAGE,
          nullptr,
          instance_,
          nullptr);
      if (dispatcher == nullptr) throw_windows_error("CreateWindowExW");
      dispatcher_window_.store(dispatcher, std::memory_order_release);
      signal_startup();

      MSG message{};
      while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    } catch (...) {
      if (!startup_complete_) signal_startup(std::current_exception());
    }

    close_all();
    const HWND dispatcher =
        dispatcher_window_.exchange(nullptr, std::memory_order_acq_rel);
    if (dispatcher != nullptr && IsWindow(dispatcher)) {
      DestroyWindow(dispatcher);
    }
    wic_factory_.Reset();
    if (com_initialized) CoUninitialize();
  }

  void invoke_sync(std::function<void()> callback) {
    if (GetCurrentThreadId() == message_thread_id_.load()) {
      callback();
      return;
    }
    const HWND dispatcher =
        dispatcher_window_.load(std::memory_order_acquire);
    if (dispatcher == nullptr || !IsWindow(dispatcher)) {
      throw std::runtime_error("overlay message thread is unavailable");
    }
    SyncInvocation invocation{std::move(callback), nullptr};
    if (SendMessageW(
            dispatcher,
            kInvokeMessage,
            0,
            reinterpret_cast<LPARAM>(&invocation)) != 1) {
      throw std::runtime_error("overlay message thread invocation failed");
    }
    if (invocation.exception != nullptr) {
      std::rethrow_exception(invocation.exception);
    }
  }

  DecodedBitmap decode_bitmap(
      const std::string& data_url,
      const std::optional<std::string>& app_icon_path,
      const OverlayHost& host,
      double max_size,
      double scale_factor,
      const RECT& work_area) {
    std::vector<BYTE> encoded_image = decode_data_url(data_url);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    require_hresult(wic_factory_->CreateStream(&stream), "IWICImagingFactory::CreateStream");
    require_hresult(
        stream->InitializeFromMemory(
            encoded_image.data(), static_cast<DWORD>(encoded_image.size())),
        "IWICStream::InitializeFromMemory");
    require_hresult(
        wic_factory_->CreateDecoderFromStream(
            stream.Get(),
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &decoder),
        "IWICImagingFactory::CreateDecoderFromStream");
    require_hresult(decoder->GetFrame(0, &frame), "IWICBitmapDecoder::GetFrame");

    UINT image_width = 0;
    UINT image_height = 0;
    require_hresult(
        frame->GetSize(&image_width, &image_height),
        "IWICBitmapFrameDecode::GetSize");
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(image_width) * image_height;
    if (image_width == 0 || image_height == 0 ||
        image_width > kMaximumImageDimension ||
        image_height > kMaximumImageDimension ||
        pixel_count > kMaximumDecodedBytes / 4) {
      throw std::runtime_error("overlay image dimensions exceed the limit");
    }

    const SIZE content_size =
        fitted_size(
            image_width,
            image_height,
            host,
            max_size,
            scale_factor,
            work_area);
    const SIZE target_size{
        std::min<LONG>(
            work_area.right - work_area.left,
            std::max<LONG>(
                content_size.cx, scaled_pixels(64, scale_factor))),
        std::min<LONG>(
            work_area.bottom - work_area.top,
            std::max<LONG>(
                content_size.cy, scaled_pixels(64, scale_factor))),
    };
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    require_hresult(
        wic_factory_->CreateBitmapScaler(&scaler),
        "IWICImagingFactory::CreateBitmapScaler");
    require_hresult(
        scaler->Initialize(
            frame.Get(),
            static_cast<UINT>(content_size.cx),
            static_cast<UINT>(content_size.cy),
            WICBitmapInterpolationModeFant),
        "IWICBitmapScaler::Initialize");
    require_hresult(
        wic_factory_->CreateFormatConverter(&converter),
        "IWICImagingFactory::CreateFormatConverter");
    require_hresult(
        converter->Initialize(
            scaler.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0,
            WICBitmapPaletteTypeCustom),
        "IWICFormatConverter::Initialize");

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = target_size.cx;
    bitmap_info.bmiHeader.biHeight = -target_size.cy;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixel_data = nullptr;
    UniqueBitmap bitmap(CreateDIBSection(
        nullptr,
        &bitmap_info,
        DIB_RGB_COLORS,
        &pixel_data,
        nullptr,
        0));
    if (bitmap.get() == nullptr || pixel_data == nullptr) {
      throw_windows_error("CreateDIBSection");
    }
    const UINT stride = static_cast<UINT>(target_size.cx) * 4;
    const UINT byte_count = stride * static_cast<UINT>(target_size.cy);
    std::memset(pixel_data, 0, byte_count);
    const UINT content_stride = static_cast<UINT>(content_size.cx) * 4;
    std::vector<BYTE> content_pixels(
        static_cast<std::size_t>(content_stride) * content_size.cy);
    require_hresult(
        converter->CopyPixels(
            nullptr,
            content_stride,
            static_cast<UINT>(content_pixels.size()),
            content_pixels.data()),
        "IWICFormatConverter::CopyPixels");
    blend_pbgra_image(
        static_cast<BYTE*>(pixel_data),
        target_size.cx,
        content_pixels.data(),
        content_size.cx,
        content_size.cy,
        (target_size.cx - content_size.cx) / 2,
        (target_size.cy - content_size.cy) / 2);
    draw_app_icon(
        static_cast<BYTE*>(pixel_data),
        target_size.cx,
        target_size.cy,
        app_icon_path,
        scale_factor);
    draw_controls(
        static_cast<BYTE*>(pixel_data),
        target_size.cx,
        target_size.cy,
        scale_factor);
    return {std::move(bitmap), target_size.cx, target_size.cy};
  }

  void draw_app_icon(
      BYTE* destination,
      int destination_width,
      int destination_height,
      const std::optional<std::string>& app_icon_path,
      double scale_factor) {
    if (!app_icon_path) return;
    const int margin = scaled_pixels(kIconMargin, scale_factor);
    const int available =
        std::min(destination_width, destination_height) - margin * 2;
    const int icon_size =
        std::min(scaled_pixels(kIconSize, scale_factor), available);
    if (icon_size <= 0) return;

    const auto data_url =
        icon_to_png_data_url(wide_string(*app_icon_path), icon_size);
    if (!data_url) return;
    std::vector<BYTE> encoded_image = decode_data_url(*data_url);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    require_hresult(
        wic_factory_->CreateStream(&stream),
        "IWICImagingFactory::CreateStream");
    require_hresult(
        stream->InitializeFromMemory(
            encoded_image.data(), static_cast<DWORD>(encoded_image.size())),
        "IWICStream::InitializeFromMemory");
    require_hresult(
        wic_factory_->CreateDecoderFromStream(
            stream.Get(),
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &decoder),
        "IWICImagingFactory::CreateDecoderFromStream");
    require_hresult(
        decoder->GetFrame(0, &frame), "IWICBitmapDecoder::GetFrame");
    require_hresult(
        wic_factory_->CreateBitmapScaler(&scaler),
        "IWICImagingFactory::CreateBitmapScaler");
    require_hresult(
        scaler->Initialize(
            frame.Get(),
            static_cast<UINT>(icon_size),
            static_cast<UINT>(icon_size),
            WICBitmapInterpolationModeFant),
        "IWICBitmapScaler::Initialize");
    require_hresult(
        wic_factory_->CreateFormatConverter(&converter),
        "IWICImagingFactory::CreateFormatConverter");
    require_hresult(
        converter->Initialize(
            scaler.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0,
            WICBitmapPaletteTypeCustom),
        "IWICFormatConverter::Initialize");

    const UINT stride = static_cast<UINT>(icon_size) * 4;
    std::vector<BYTE> icon_pixels(
        static_cast<std::size_t>(stride) * icon_size);
    require_hresult(
        converter->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(icon_pixels.size()),
            icon_pixels.data()),
        "IWICFormatConverter::CopyPixels");
    blend_pbgra_image(
        destination,
        destination_width,
        icon_pixels.data(),
        icon_size,
        icon_size,
        margin,
        destination_height - margin - icon_size);
  }

  HWND create_tooltip_window(WindowState& state) {
    state.tooltip_window = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        state.window,
        nullptr,
        instance_,
        nullptr);
    if (state.tooltip_window == nullptr) {
      throw_windows_error("CreateWindowExW(tooltip)");
    }
    SetWindowPos(
        state.tooltip_window,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    TOOLINFOW hide = tooltip_info(
        state,
        kHideTooltipId,
        state.controls.hide,
        state.hide_tooltip.data());
    TOOLINFOW relocate = tooltip_info(
        state,
        kRelocateTooltipId,
        state.controls.relocate,
        state.relocate_tooltip.data());
    if (!SendMessageW(
            state.tooltip_window,
            TTM_ADDTOOLW,
            0,
            reinterpret_cast<LPARAM>(&hide)) ||
        !SendMessageW(
            state.tooltip_window,
            TTM_ADDTOOLW,
            0,
            reinterpret_cast<LPARAM>(&relocate))) {
      throw std::runtime_error("failed to add overlay tooltip");
    }
    return state.tooltip_window;
  }

  HWND create_overlay_window(const RenderItem& item) {
    auto state = std::make_unique<WindowState>();
    state->events = &events_;
    state->host_id = item.host_id;
    state->controls = control_rects(
        item.image.width, item.image.height, item.scale_factor);
    state->hide_tooltip = item.hide_tooltip;
    state->relocate_tooltip = item.relocate_tooltip;
    const HWND window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kOverlayClassName,
        item.title.c_str(),
        WS_POPUP,
        item.frame.left,
        item.frame.top,
        item.image.width,
        item.image.height,
        nullptr,
        nullptr,
        instance_,
        state.get());
    if (window == nullptr) throw_windows_error("CreateWindowExW");
    state->window = window;
    try {
      create_tooltip_window(*state);
    } catch (...) {
      DestroyWindow(window);
      throw;
    }
    windows_.emplace(item.id, std::move(state));
    return window;
  }

  void present_bitmap(const RenderItem& item, HWND window) {
    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) throw_windows_error("GetDC");
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
      ReleaseDC(nullptr, screen_dc);
      throw_windows_error("CreateCompatibleDC");
    }
    const HGDIOBJ previous = SelectObject(memory_dc, item.image.bitmap.get());
    if (previous == nullptr || previous == HGDI_ERROR) {
      DeleteDC(memory_dc);
      ReleaseDC(nullptr, screen_dc);
      throw_windows_error("SelectObject");
    }

    const POINT destination{item.frame.left, item.frame.top};
    const POINT source{0, 0};
    const SIZE size{item.image.width, item.image.height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(
        window,
        screen_dc,
        &destination,
        &size,
        memory_dc,
        &source,
        0,
        &blend,
        ULW_ALPHA);
    SelectObject(memory_dc, previous);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    if (!updated) throw_windows_error("UpdateLayeredWindow");

    UINT flags = SWP_NOACTIVATE;
    if (item.visible) flags |= SWP_SHOWWINDOW;
    if (!SetWindowPos(
            window,
            HWND_TOPMOST,
            item.frame.left,
            item.frame.top,
            item.image.width,
            item.image.height,
            flags)) {
      throw_windows_error("SetWindowPos");
    }
    if (!item.visible) ShowWindow(window, SW_HIDE);
  }

  void apply_update(const OverlaySnapshot& snapshot) {
    std::unordered_map<std::string, const OverlayHost*> hosts;
    hosts.reserve(snapshot.hosts.size());
    for (const auto& host : snapshot.hosts) hosts.emplace(host.id, &host);

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
                         HostLayout{
                             work_area_for_host(*host->second),
                             0,
                             scale_for_host(*host->second)})
                     .first;
      }

      RenderItem item;
      item.id = presentation.id;
      item.host_id = presentation.host_id;
      item.title = wide_string(host->second->title);
      item.hide_tooltip = wide_string(snapshot.options.hide_tooltip);
      item.relocate_tooltip =
          wide_string(snapshot.options.relocate_tooltip);
      item.scale_factor = layout->second.scale_factor;
      item.image = decode_bitmap(
          presentation.image_data,
          presentation.app_icon_path,
          *host->second,
          snapshot.max_size,
          layout->second.scale_factor,
          layout->second.work_area);
      item.frame = presentation_frame(
          *host->second,
          {item.image.width, item.image.height},
          layout->second.cursor,
          layout->second.scale_factor,
          layout->second.work_area);
      item.visible = presentation.visible && snapshot.visible &&
                     presentation_fits(
                         *host->second,
                         {item.image.width, item.image.height},
                         layout->second.cursor,
                         layout->second.scale_factor,
                         layout->second.work_area);
      if (item.visible) {
        layout->second.cursor +=
            (host->second->anchor.edge == AnchorEdge::kLeading ||
             host->second->anchor.edge == AnchorEdge::kTrailing)
                ? item.image.height + kStackGap * layout->second.scale_factor
                : item.image.width + kStackGap * layout->second.scale_factor;
      }
      items.push_back(std::move(item));
    }

    for (auto item = items.rbegin(); item != items.rend(); ++item) {
      auto existing = windows_.find(item->id);
      HWND window = nullptr;
      if (existing == windows_.end()) {
        window = create_overlay_window(*item);
        existing = windows_.find(item->id);
      } else {
        window = existing->second->window;
        existing->second->host_id = item->host_id;
        existing->second->controls = control_rects(
            item->image.width,
            item->image.height,
            item->scale_factor);
        update_tooltips(
            *existing->second,
            item->hide_tooltip,
            item->relocate_tooltip);
        SetWindowTextW(window, item->title.c_str());
      }
      present_bitmap(*item, window);
    }

    for (auto iterator = windows_.begin(); iterator != windows_.end();) {
      if (active_ids.find(iterator->first) != active_ids.end()) {
        ++iterator;
        continue;
      }
      if (iterator->second->window != nullptr) {
        DestroyWindow(iterator->second->window);
      }
      iterator = windows_.erase(iterator);
    }
  }

  void close_all() noexcept {
    for (auto& [id, state] : windows_) {
      if (state->window != nullptr && IsWindow(state->window)) {
        DestroyWindow(state->window);
      }
    }
    windows_.clear();
  }

  OverlayPlatformEvents events_;
  std::thread message_thread_;
  std::mutex startup_mutex_;
  std::condition_variable startup_condition_;
  bool startup_complete_ = false;
  std::exception_ptr startup_error_;
  std::atomic<bool> stopped_{false};
  std::atomic<DWORD> message_thread_id_{0};
  std::atomic<HWND> dispatcher_window_{nullptr};
  HINSTANCE instance_ = nullptr;
  ComPtr<IWICImagingFactory> wic_factory_;
  std::unordered_map<std::string, std::unique_ptr<WindowState>> windows_;
};

}  // namespace

std::unique_ptr<OverlayPlatform> create_overlay_platform(
    OverlayPlatformEvents events) {
  return std::make_unique<WindowsOverlayPlatform>(std::move(events));
}

}  // namespace nativekit::platform
