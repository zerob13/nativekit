#include "drag/drag_source.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "common/event_callback.h"
#include "common/napi_helpers.h"

namespace nativekit {
namespace {

class DragManager {
 public:
  bool start(const DragRequest& request) {
    ensure_platform();
    return platform_->start(request);
  }

  EventCallback& ended_callback() { return ended_callback_; }

  void cleanup() noexcept {
    try {
      if (platform_) platform_->stop();
    } catch (...) {
    }
    platform_.reset();
    try {
      ended_callback_.reset();
    } catch (...) {
    }
  }

 private:
  void ensure_platform() {
    if (platform_) return;
    platform_ = platform::create_drag_platform({[this](DragResult result) {
      ended_callback_.emit(DragEndedEvent{
          result.dropped, result.position.x, result.position.y});
    }});
  }

  std::unique_ptr<DragPlatform> platform_;
  EventCallback ended_callback_;
};

DragManager manager;

std::string string_arg(const Napi::Value& value, const char* name) {
  if (!value.IsString()) {
    throw std::invalid_argument(std::string(name) + " must be a string");
  }
  const std::string result = value.As<Napi::String>().Utf8Value();
  if (result.empty() || result.find('\0') != std::string::npos) {
    throw std::invalid_argument(std::string(name) + " must be valid");
  }
  return result;
}

double finite_number(const Napi::Object& object, const char* name) {
  const Napi::Value value = object.Get(name);
  if (!value.IsNumber()) {
    throw std::invalid_argument(std::string("position.") + name +
                                " must be a number");
  }
  const double result = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(result)) {
    throw std::invalid_argument(std::string("position.") + name +
                                " must be finite");
  }
  return result;
}

DragRequest request_arg(const Napi::CallbackInfo& info) {
  if (info.Length() != 1 || !info[0].IsObject()) {
    throw std::invalid_argument("config must be an object");
  }
  const Napi::Object config = info[0].As<Napi::Object>();
  const Napi::Value files_value = config.Get("files");
  if (!files_value.IsArray()) {
    throw std::invalid_argument("files must be a non-empty array");
  }
  const Napi::Array files = files_value.As<Napi::Array>();
  if (files.Length() == 0) {
    throw std::invalid_argument("files must be a non-empty array");
  }

  DragRequest request;
  request.files.reserve(files.Length());
  for (std::uint32_t index = 0; index < files.Length(); ++index) {
    request.files.push_back(string_arg(files.Get(index), "file"));
  }

  const Napi::Value handle_value = config.Get("windowHandle");
  if (!handle_value.IsBuffer()) {
    throw std::invalid_argument("windowHandle must be a Buffer");
  }
  const auto handle = handle_value.As<Napi::Buffer<std::uint8_t>>();
  if (handle.Length() != sizeof(std::uint64_t)) {
    throw std::invalid_argument("windowHandle has an unexpected size");
  }
  std::uint64_t handle_value_raw = 0;
  std::memcpy(&handle_value_raw, handle.Data(), sizeof(handle_value_raw));
  if (handle_value_raw == 0 || handle_value_raw > UINTPTR_MAX) {
    throw std::invalid_argument("windowHandle is invalid");
  }
  request.window_handle = static_cast<std::uintptr_t>(handle_value_raw);

  const Napi::Value position_value = config.Get("position");
  if (!position_value.IsObject()) {
    throw std::invalid_argument("position must be an object");
  }
  const Napi::Object position = position_value.As<Napi::Object>();
  request.position = {
      finite_number(position, "x"), finite_number(position, "y")};
  return request;
}

Napi::Value start(const Napi::CallbackInfo& info) {
  try {
    return Napi::Boolean::New(info.Env(), manager.start(request_arg(info)));
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  } catch (const std::exception& error) {
    throw_js_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

Napi::Value set_ended_callback(const Napi::CallbackInfo& info) {
  try {
    manager.ended_callback().set(info, "nativekit.drag.ended");
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

}  // namespace

void register_drag(Napi::Env env, Napi::Object& exports) {
  exports.Set("dragStart", Napi::Function::New(env, start));
  exports.Set("dragOnEnded", Napi::Function::New(env, set_ended_callback));
}

void cleanup_drag() noexcept {
  manager.cleanup();
}

}  // namespace nativekit
