#pragma once

#include <napi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/event_callback.h"

namespace nativekit {

struct SecureChannelEvents {
  std::function<void(SecureChannelEvent)> emit;
};

class SecureChannelPlatform {
 public:
  virtual ~SecureChannelPlatform() = default;
  virtual std::optional<std::uint32_t> spawn(
      const std::string& executable_path,
      const std::vector<std::string>& arguments) = 0;
  virtual bool verify(
      std::uint32_t pid,
      const std::string& executable_path) const = 0;
  virtual bool active() const = 0;
  virtual void stop() = 0;
};

namespace platform {

std::unique_ptr<SecureChannelPlatform> create_secure_channel_platform(
    SecureChannelEvents events);

}  // namespace platform

void register_secure_channel(Napi::Env env, Napi::Object& exports);
void cleanup_secure_channel() noexcept;

}  // namespace nativekit
