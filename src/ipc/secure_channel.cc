#include "ipc/secure_channel.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "common/event_callback.h"
#include "common/napi_helpers.h"

namespace nativekit {
namespace {

class SecureChannelManager {
 public:
  std::optional<std::uint32_t> spawn(
      const std::string& executable_path,
      const std::vector<std::string>& arguments) {
    ensure_platform();
    return platform_->spawn(executable_path, arguments);
  }

  bool verify(std::uint32_t pid, const std::string& executable_path) {
    ensure_platform();
    return platform_->verify(pid, executable_path);
  }

  bool terminate() {
    if (!platform_ || !platform_->active()) return false;
    platform_->stop();
    platform_.reset();
    return true;
  }

  EventCallback& event_callback() { return event_callback_; }

  void cleanup() noexcept {
    try {
      if (platform_) platform_->stop();
      platform_.reset();
    } catch (...) {
      (void)platform_.release();
    }
    try {
      event_callback_.reset();
    } catch (...) {
    }
  }

 private:
  void ensure_platform() {
    if (platform_) return;
    platform_ = platform::create_secure_channel_platform({
        [this](SecureChannelEvent event) {
          event_callback_.emit(std::move(event));
        },
    });
  }

  std::unique_ptr<SecureChannelPlatform> platform_;
  EventCallback event_callback_;
};

SecureChannelManager manager;

std::string path_arg(const Napi::CallbackInfo& info, std::size_t index) {
  if (info.Length() <= index || !info[index].IsString()) {
    throw std::invalid_argument("executablePath must be a string");
  }
  const std::string path = info[index].As<Napi::String>().Utf8Value();
  if (path.empty() || path.find('\0') != std::string::npos) {
    throw std::invalid_argument("executablePath must be a valid path");
  }
  return path;
}

std::vector<std::string> arguments_arg(
    const Napi::CallbackInfo& info,
    std::size_t index) {
  if (info.Length() <= index || info[index].IsUndefined()) return {};
  if (!info[index].IsArray()) {
    throw std::invalid_argument("arguments must be an array of strings");
  }
  const Napi::Array array = info[index].As<Napi::Array>();
  std::vector<std::string> arguments;
  arguments.reserve(array.Length());
  for (std::uint32_t item = 0; item < array.Length(); ++item) {
    const Napi::Value value = array.Get(item);
    if (!value.IsString()) {
      throw std::invalid_argument("arguments must be an array of strings");
    }
    std::string argument = value.As<Napi::String>().Utf8Value();
    if (argument.find('\0') != std::string::npos) {
      throw std::invalid_argument("arguments must not contain null bytes");
    }
    arguments.push_back(std::move(argument));
  }
  return arguments;
}

Napi::Value spawn(const Napi::CallbackInfo& info) {
  try {
    const auto pid = manager.spawn(path_arg(info, 0), arguments_arg(info, 1));
    return pid ? Napi::Number::New(info.Env(), *pid) : info.Env().Null();
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  } catch (const std::exception& error) {
    throw_js_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

Napi::Value verify(const Napi::CallbackInfo& info) {
  try {
    const WindowId pid_value = window_id_arg(info, 0, "pid");
    if (pid_value > UINT32_MAX) {
      throw std::invalid_argument("pid is out of range");
    }
    return Napi::Boolean::New(
        info.Env(),
        manager.verify(
            static_cast<std::uint32_t>(pid_value), path_arg(info, 1)));
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  } catch (const std::exception& error) {
    throw_js_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

Napi::Value terminate(const Napi::CallbackInfo& info) {
  try {
    return Napi::Boolean::New(info.Env(), manager.terminate());
  } catch (const std::exception& error) {
    throw_js_error(info.Env(), error);
    return info.Env().Undefined();
  }
}

template <EventCallback& (SecureChannelManager::*Getter)()>
Napi::Value set_callback(const Napi::CallbackInfo& info, const char* name) {
  try {
    (manager.*Getter)().set(info, name);
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

}  // namespace

void register_secure_channel(Napi::Env env, Napi::Object& exports) {
  exports.Set("secureChannelSpawn", Napi::Function::New(env, spawn));
  exports.Set("secureChannelVerify", Napi::Function::New(env, verify));
  exports.Set("secureChannelTerminate", Napi::Function::New(env, terminate));
  exports.Set(
      "secureChannelOnEvent",
      Napi::Function::New(
          env,
          [](const Napi::CallbackInfo& info) {
            return set_callback<&SecureChannelManager::event_callback>(
                info, "nativekit.secureChannel.event");
          }));
}

void cleanup_secure_channel() noexcept {
  manager.cleanup();
}

}  // namespace nativekit
