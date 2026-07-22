#pragma once

#include <napi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace nativekit {

enum class AnchorEdge { kLeading, kTrailing, kTop, kBottom };

struct Anchor {
  AnchorEdge edge = AnchorEdge::kTrailing;
  double offset = 16;
};

struct OverlayHost {
  std::string id;
  std::string title;
  Rect bounds;
  std::uintptr_t window_handle = 0;
  Anchor anchor;
};

struct OverlayPresentation {
  std::string id;
  std::string session_id;
  std::string host_id;
  std::string image_data;
  std::optional<std::string> app_icon_path;
  bool visible = true;
  std::size_t order = 0;
};

struct OverlayOptions {
  std::string hide_tooltip = "Hide";
  std::string relocate_tooltip = "Move";
};

struct OverlaySnapshot {
  std::vector<OverlayHost> hosts;
  std::vector<OverlayPresentation> presentations;
  OverlayOptions options;
  bool visible = true;
  double max_size = 480;
};

struct OverlayPlatformEvents {
  std::function<void()> activate;
  std::function<void(bool)> visibility_request;
  std::function<void(const std::string&)> relocate;
};

class OverlayPlatform {
 public:
  virtual ~OverlayPlatform() = default;
  virtual void update(const OverlaySnapshot& snapshot) = 0;
  virtual void stop() = 0;
};

namespace platform {

std::unique_ptr<OverlayPlatform> create_overlay_platform(
    OverlayPlatformEvents events);

}  // namespace platform

void register_overlay(Napi::Env env, Napi::Object& exports);
void cleanup_overlay() noexcept;

}  // namespace nativekit
