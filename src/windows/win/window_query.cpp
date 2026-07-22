#include "windows/window_query.h"

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "common/win/image_utils.h"

namespace nativekit::platform {
namespace {

std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      size,
      nullptr,
      nullptr);
  return result;
}

std::wstring window_text(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring value(length + 1, L'\0');
  const int copied = GetWindowTextW(window, value.data(), length + 1);
  value.resize(std::max(copied, 0));
  return value;
}

std::wstring executable_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return {};
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) size = 0;
  CloseHandle(process);
  path.resize(size);
  return path;
}

std::optional<SystemWindow> parse_window(HWND handle, int level) {
  if (handle == nullptr || !IsWindow(handle)) return std::nullopt;

  RECT rect{};
  if (FAILED(DwmGetWindowAttribute(
          handle, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
      !GetWindowRect(handle, &rect)) {
    return std::nullopt;
  }
  if (rect.right <= rect.left || rect.bottom <= rect.top) return std::nullopt;

  DWORD pid = 0;
  GetWindowThreadProcessId(handle, &pid);
  const std::wstring path = executable_path(pid);
  const std::wstring title = window_text(handle);
  DWORD cloaked = 0;
  DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  const RECT monitor_bounds = rect;
  const bool intersects_display =
      MonitorFromRect(&monitor_bounds, MONITOR_DEFAULTTONULL) != nullptr;

  SystemWindow result;
  result.id = reinterpret_cast<std::uintptr_t>(handle);
  if (!title.empty()) result.name = utf8(title);
  result.bounds = {
      static_cast<double>(rect.left),
      static_cast<double>(rect.top),
      static_cast<double>(rect.right - rect.left),
      static_cast<double>(rect.bottom - rect.top),
  };
  result.level = level;
  result.owner_pid = pid;
  if (!path.empty()) {
    result.owner_name = utf8(std::filesystem::path(path).filename().wstring());
  }
  result.is_onscreen = IsWindowVisible(handle) && !IsIconic(handle) &&
                       cloaked == 0 && intersects_display;
  return result;
}

struct Enumeration {
  WindowId relative_to = 0;
  bool below_reference = true;
  int level = 0;
  std::vector<SystemWindow> windows;
};

BOOL CALLBACK collect_window(HWND handle, LPARAM parameter) {
  auto& context = *reinterpret_cast<Enumeration*>(parameter);
  const WindowId id = reinterpret_cast<std::uintptr_t>(handle);
  if (!context.below_reference) {
    context.below_reference = id == context.relative_to;
    ++context.level;
    return TRUE;
  }
  if (const auto window = parse_window(handle, context.level)) {
    context.windows.push_back(*window);
  }
  ++context.level;
  return TRUE;
}

}  // namespace

std::vector<SystemWindow> list_windows(WindowId relative_to) {
  Enumeration context;
  context.relative_to = relative_to;
  context.below_reference = relative_to == 0;
  EnumWindows(collect_window, reinterpret_cast<LPARAM>(&context));
  return context.windows;
}

std::optional<SystemWindow> find_window(WindowId id) {
  HWND handle = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
  if (!IsWindow(handle)) return std::nullopt;
  int level = 0;
  for (HWND cursor = GetTopWindow(nullptr);
       cursor != nullptr && cursor != handle;
       cursor = GetWindow(cursor, GW_HWNDNEXT)) {
    ++level;
  }
  return parse_window(handle, level);
}

std::optional<SystemWindow> frontmost_window() {
  HWND handle = GetForegroundWindow();
  if (handle == nullptr) return std::nullopt;
  const auto system_window = parse_window(handle, 0);
  if (!system_window) return std::nullopt;

  const std::wstring path = executable_path(system_window->owner_pid);
  FrontmostWindow result;
  result.bundle_id = utf8(path);
  result.name = system_window->owner_name.value_or(result.bundle_id);
  result.title = system_window->name;
  result.icon = icon_to_png_data_url(path, 32);
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
