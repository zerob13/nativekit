#pragma once

#include <napi.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace nativekit {

struct DragEndedEvent {
  bool dropped = false;
  double x = 0;
  double y = 0;
};

enum class SecureChannelEventType {
  kData,
  kExit,
};

struct SecureChannelEvent {
  SecureChannelEventType type = SecureChannelEventType::kData;
  std::vector<std::uint8_t> data;
  std::int32_t exit_code = 0;
};

using EventPayload = std::variant<
    std::monostate,
    bool,
    double,
    std::int32_t,
    std::string,
    std::vector<std::uint8_t>,
    DragEndedEvent,
    SecureChannelEvent>;

class EventCallback {
 public:
  EventCallback() = default;
  EventCallback(const EventCallback&) = delete;
  EventCallback& operator=(const EventCallback&) = delete;
  ~EventCallback();

  void set(const Napi::CallbackInfo& info, const std::string& resource_name);
  void set(
      Napi::Env env,
      const Napi::Function& callback,
      const std::string& resource_name);
  void emit(EventPayload payload = std::monostate{});
  void reset();

 private:
  std::mutex mutex_;
  Napi::ThreadSafeFunction callback_;
  bool active_ = false;
};

}  // namespace nativekit
