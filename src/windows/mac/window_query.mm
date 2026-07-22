#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include "windows/window_query.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <type_traits>

#include "common/mac/image_utils.h"

namespace nativekit::platform {
namespace {

std::optional<std::string> cf_string(CFDictionaryRef dictionary, CFStringRef key) {
  CFTypeRef value = CFDictionaryGetValue(dictionary, key);
  if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
    return std::nullopt;
  }
  NSString* string = (__bridge NSString*)value;
  return std::string(string.UTF8String ?: "");
}

template <typename Number>
Number cf_number(CFDictionaryRef dictionary, CFStringRef key, Number fallback = 0) {
  CFTypeRef value = CFDictionaryGetValue(dictionary, key);
  if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
    return fallback;
  }
  if constexpr (std::is_floating_point_v<Number>) {
    double output = 0;
    CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &output);
    return static_cast<Number>(output);
  } else {
    long long output = 0;
    CFNumberGetValue((CFNumberRef)value, kCFNumberLongLongType, &output);
    return static_cast<Number>(output);
  }
}

std::optional<SystemWindow> parse_window(CFDictionaryRef dictionary) {
  CGRect bounds = CGRectZero;
  CFTypeRef bounds_value =
      CFDictionaryGetValue(dictionary, kCGWindowBounds);
  if (bounds_value == nullptr ||
      CFGetTypeID(bounds_value) != CFDictionaryGetTypeID() ||
      !CGRectMakeWithDictionaryRepresentation(
          (CFDictionaryRef)bounds_value, &bounds) ||
      bounds.size.width <= 0 || bounds.size.height <= 0) {
    return std::nullopt;
  }

  SystemWindow window;
  window.id = cf_number<WindowId>(dictionary, kCGWindowNumber);
  window.name = cf_string(dictionary, kCGWindowName);
  window.bounds = {
      bounds.origin.x,
      bounds.origin.y,
      bounds.size.width,
      bounds.size.height,
  };
  window.level = cf_number<int>(dictionary, kCGWindowLayer);
  window.owner_pid = cf_number<std::uint32_t>(dictionary, kCGWindowOwnerPID);
  window.owner_name = cf_string(dictionary, kCGWindowOwnerName);
  window.is_onscreen =
      cf_number<int>(dictionary, kCGWindowIsOnscreen, 0) != 0;
  return window.id == 0 ? std::nullopt : std::optional(window);
}

std::vector<SystemWindow> copy_windows(
    CGWindowListOption option,
    CGWindowID relative_to,
    bool normalize_levels = true) {
  CFArrayRef descriptions =
      CGWindowListCopyWindowInfo(option, relative_to);
  if (descriptions == nullptr) return {};

  std::vector<SystemWindow> windows;
  windows.reserve(CFArrayGetCount(descriptions));
  for (CFIndex index = 0; index < CFArrayGetCount(descriptions); ++index) {
    CFDictionaryRef dictionary = (CFDictionaryRef)CFArrayGetValueAtIndex(
        descriptions, index);
    if (auto window = parse_window(dictionary)) {
      if (normalize_levels) {
        window->level = static_cast<int>(windows.size());
      }
      windows.push_back(std::move(*window));
    }
  }
  CFRelease(descriptions);
  return windows;
}

}  // namespace

std::optional<FrontmostWindow> frontmost_window() {
  @autoreleasepool {
    NSRunningApplication* application =
        NSWorkspace.sharedWorkspace.frontmostApplication;
    if (application == nil) return std::nullopt;

    FrontmostWindow result;
    NSString* identifier = application.bundleIdentifier;
    if (identifier.length == 0) identifier = application.bundleURL.path;
    result.bundle_id = std::string(identifier.UTF8String ?: "");
    result.name = std::string(application.localizedName.UTF8String ?: "");
    result.icon = image_to_png_data_url(application.icon, 32);

    const pid_t pid = application.processIdentifier;
    const auto windows = copy_windows(
        kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID,
        false);
    const auto match = std::find_if(
        windows.begin(), windows.end(), [pid](const SystemWindow& window) {
          return window.owner_pid == static_cast<std::uint32_t>(pid) &&
                 window.level == 0 && window.is_onscreen;
        });
    if (match != windows.end()) result.title = match->name;
    return result;
  }
}

std::vector<SystemWindow> list_windows(WindowId relative_to) {
  @autoreleasepool {
    auto windows = copy_windows(
        kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (relative_to == 0) return windows;
    const auto reference = std::find_if(
        windows.begin(), windows.end(), [relative_to](const SystemWindow& window) {
          return window.id == relative_to;
        });
    if (reference == windows.end()) return {};
    windows.erase(windows.begin(), std::next(reference));
    return windows;
  }
}

std::optional<SystemWindow> find_window(WindowId id) {
  @autoreleasepool {
    const auto windows = copy_windows(
        kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    const auto match = std::find_if(
        windows.begin(), windows.end(), [id](const SystemWindow& window) {
          return window.id == id;
        });
    return match == windows.end() ? std::nullopt : std::optional(*match);
  }
}

std::optional<SystemWindow> window_at_point(Point point, WindowId below_id) {
  const auto windows = list_windows(0);
  bool can_match = below_id == 0;
  for (const auto& window : windows) {
    if (!can_match) {
      can_match = window.id == below_id;
      continue;
    }
    if (window.is_onscreen && window.bounds.contains(point)) return window;
  }
  return std::nullopt;
}

}  // namespace nativekit::platform
