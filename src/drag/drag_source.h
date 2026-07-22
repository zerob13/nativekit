#pragma once

#include <napi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"

namespace nativekit {

struct DragRequest {
  std::vector<std::string> files;
  std::uintptr_t window_handle = 0;
  Point position;
};

struct DragResult {
  bool dropped = false;
  Point position;
};

struct DragEvents {
  std::function<void(DragResult)> ended;
};

class DragPlatform {
 public:
  virtual ~DragPlatform() = default;
  virtual bool start(const DragRequest& request) = 0;
  virtual void stop() = 0;
};

namespace platform {

std::unique_ptr<DragPlatform> create_drag_platform(DragEvents events);

}  // namespace platform

void register_drag(Napi::Env env, Napi::Object& exports);
void cleanup_drag() noexcept;

}  // namespace nativekit
