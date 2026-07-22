#include "common/event_callback.h"

#include <memory>
#include <stdexcept>
#include <type_traits>

namespace nativekit {
namespace {

void call_javascript(
    Napi::Env env,
    Napi::Function callback,
    EventPayload* payload) {
  std::unique_ptr<EventPayload> owned(payload);
  std::visit(
      [&env, &callback](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          callback.Call({});
        } else if constexpr (std::is_same_v<Value, bool>) {
          callback.Call({Napi::Boolean::New(env, value)});
        } else if constexpr (
            std::is_same_v<Value, double> ||
            std::is_same_v<Value, std::int32_t>) {
          callback.Call({Napi::Number::New(env, value)});
        } else if constexpr (std::is_same_v<Value, std::string>) {
          callback.Call({Napi::String::New(env, value)});
        } else if constexpr (std::is_same_v<Value, std::vector<std::uint8_t>>) {
          callback.Call({Napi::Buffer<std::uint8_t>::Copy(
              env, value.data(), value.size())});
        } else if constexpr (std::is_same_v<Value, DragEndedEvent>) {
          Napi::Object result = Napi::Object::New(env);
          result.Set("dropped", value.dropped);
          result.Set("x", value.x);
          result.Set("y", value.y);
          callback.Call({result});
        } else if constexpr (std::is_same_v<Value, SecureChannelEvent>) {
          if (value.type == SecureChannelEventType::kData) {
            callback.Call({
                Napi::String::New(env, "data"),
                Napi::Buffer<std::uint8_t>::Copy(
                    env, value.data.data(), value.data.size()),
            });
          } else {
            callback.Call({
                Napi::String::New(env, "exit"),
                Napi::Number::New(env, value.exit_code),
            });
          }
        }
      },
      *owned);
}

}  // namespace

EventCallback::~EventCallback() {
  reset();
}

void EventCallback::set(
    const Napi::CallbackInfo& info,
    const std::string& resource_name) {
  if (info.Length() != 1 || !info[0].IsFunction()) {
    throw std::invalid_argument("callback must be a function");
  }
  set(info.Env(), info[0].As<Napi::Function>(), resource_name);
}

void EventCallback::set(
    Napi::Env env,
    const Napi::Function& callback,
    const std::string& resource_name) {
  std::lock_guard lock(mutex_);
  if (active_) callback_.Release();
  callback_ = Napi::ThreadSafeFunction::New(
      env, callback, resource_name, 0, 1);
  callback_.Unref(env);
  active_ = true;
}

void EventCallback::emit(EventPayload payload) {
  auto* owned = new EventPayload(std::move(payload));
  std::lock_guard lock(mutex_);
  if (!active_ ||
      callback_.NonBlockingCall(owned, call_javascript) != napi_ok) {
    delete owned;
  }
}

void EventCallback::reset() {
  std::lock_guard lock(mutex_);
  if (!active_) return;
  callback_.Release();
  active_ = false;
  callback_ = Napi::ThreadSafeFunction();
}

}  // namespace nativekit
