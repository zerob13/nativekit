#include "overlay/overlay_manager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/event_callback.h"
#include "common/napi_helpers.h"

namespace nativekit {
namespace {

constexpr std::size_t kMaximumImageDataLength = 32 * 1024 * 1024;

AnchorEdge parse_edge(const std::string& edge) {
  if (edge == "leading") return AnchorEdge::kLeading;
  if (edge == "trailing") return AnchorEdge::kTrailing;
  if (edge == "top") return AnchorEdge::kTop;
  if (edge == "bottom") return AnchorEdge::kBottom;
  throw std::invalid_argument("anchor.edge is invalid");
}

std::string required_string(const Napi::Object& object, const char* name) {
  const Napi::Value value = object.Get(name);
  if (!value.IsString()) {
    throw std::invalid_argument(std::string(name) + " must be a string");
  }
  const std::string result = value.As<Napi::String>().Utf8Value();
  if (result.empty()) {
    throw std::invalid_argument(std::string(name) + " must not be empty");
  }
  return result;
}

double required_number(const Napi::Object& object, const char* name) {
  const Napi::Value value = object.Get(name);
  if (!value.IsNumber()) {
    throw std::invalid_argument(std::string(name) + " must be a number");
  }
  const double result = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(result)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
  return result;
}

Rect parse_rect(const Napi::Value& value) {
  if (!value.IsObject()) throw std::invalid_argument("bounds must be an object");
  const Napi::Object object = value.As<Napi::Object>();
  Rect result{
      required_number(object, "x"),
      required_number(object, "y"),
      required_number(object, "width"),
      required_number(object, "height"),
  };
  if (result.width <= 0 || result.height <= 0) {
    throw std::invalid_argument("bounds width and height must be positive");
  }
  return result;
}

Anchor parse_anchor(const Napi::Value& value) {
  if (!value.IsObject()) throw std::invalid_argument("anchor must be an object");
  const Napi::Object object = value.As<Napi::Object>();
  const double offset = required_number(object, "offset");
  if (offset < 0) throw std::invalid_argument("anchor.offset must be non-negative");
  return {parse_edge(required_string(object, "edge")), offset};
}

std::uintptr_t parse_window_handle(const Napi::Value& value) {
  if (!value.IsBuffer()) {
    throw std::invalid_argument("windowHandle must be a Buffer");
  }
  const auto buffer = value.As<Napi::Buffer<std::uint8_t>>();
#if defined(__linux__)
  if (buffer.Length() != sizeof(std::uint32_t)) {
    throw std::invalid_argument("windowHandle has an unexpected size");
  }
  std::uint32_t x_window = 0;
  std::memcpy(&x_window, buffer.Data(), sizeof(x_window));
  const std::uintptr_t handle = x_window;
#else
  if (buffer.Length() != sizeof(std::uintptr_t)) {
    throw std::invalid_argument("windowHandle has an unexpected size");
  }
  std::uintptr_t handle = 0;
  std::memcpy(&handle, buffer.Data(), sizeof(handle));
#endif
  if (handle == 0) throw std::invalid_argument("windowHandle is null");
  return handle;
}

class OverlayManager {
 public:
  void initialize(Napi::Env env) {
    visibility_dispatcher_.set(
        env,
        Napi::Function::New(env, [this](const Napi::CallbackInfo& info) {
          if (info.Length() == 1 && info[0].IsBoolean()) {
            handle_visibility_request(info[0].As<Napi::Boolean>());
          }
        }),
        "nativekit.overlay.visibilityDispatcher");
    relocate_dispatcher_.set(
        env,
        Napi::Function::New(env, [this](const Napi::CallbackInfo& info) {
          if (info.Length() == 1 && info[0].IsString()) {
            relocate(info[0].As<Napi::String>());
          }
        }),
        "nativekit.overlay.relocateDispatcher");
    refresh_dispatcher_.set(
        env,
        Napi::Function::New(env, [this](const Napi::CallbackInfo&) {
          if (!running_) return;
          try {
            sync();
          } catch (...) {
            // A best-effort DPI refresh must not unwind through N-API.
          }
        }),
        "nativekit.overlay.refreshDispatcher");
  }

  bool start(OverlayOptions options) {
    options_ = std::move(options);
    if (!platform_) {
      platform_ = platform::create_overlay_platform({
          [this] { activate_callback_.emit(); },
          [this](bool visible) { visibility_dispatcher_.emit(visible); },
          [this](const std::string& host_id) {
            relocate_dispatcher_.emit(host_id);
          },
          [this] { refresh_dispatcher_.emit(); },
      });
    }
    running_ = true;
    sync();
    return true;
  }

  bool stop() {
    if (platform_) platform_->stop();
    platform_.reset();
    hosts_.clear();
    host_order_.clear();
    presentations_.clear();
    presentation_order_.clear();
    suppressed_sessions_.clear();
    active_session_.clear();
    running_ = false;
    visible_ = true;
    return true;
  }

  bool attach_host(OverlayHost host) {
    require_running();
    if (hosts_.find(host.id) == hosts_.end()) host_order_.push_back(host.id);
    hosts_.insert_or_assign(host.id, std::move(host));
    sync();
    return true;
  }

  bool detach_host(const std::string& host_id) {
    require_running();
    if (hosts_.erase(host_id) == 0) return false;
    host_order_.erase(
        std::remove(host_order_.begin(), host_order_.end(), host_id),
        host_order_.end());
    for (auto iterator = presentation_order_.begin();
         iterator != presentation_order_.end();) {
      const auto presentation = presentations_.find(*iterator);
      if (presentation != presentations_.end() &&
          presentation->second.host_id == host_id) {
        presentations_.erase(presentation);
        iterator = presentation_order_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    sync();
    return true;
  }

  bool set_visible(bool visible) {
    require_running();
    visible_ = visible;
    sync();
    return true;
  }

  bool set_max_size(double size) {
    require_running();
    if (!std::isfinite(size) || size <= 0) {
      throw std::invalid_argument("size must be positive");
    }
    max_size_ = size;
    sync();
    max_size_callback_.emit(size);
    return true;
  }

  bool push_image(OverlayPresentation presentation) {
    require_running();
    if (presentation.image_data.size() > kMaximumImageDataLength) {
      throw std::invalid_argument("imageData exceeds the 32 MiB limit");
    }
    if (presentation.host_id.empty()) {
      if (host_order_.size() != 1) {
        throw std::invalid_argument(
            "hostId is required when more than one host is attached");
      }
      presentation.host_id = host_order_.front();
    }
    if (hosts_.find(presentation.host_id) == hosts_.end()) {
      throw std::invalid_argument("hostId is not attached");
    }

    const auto existing = presentations_.find(presentation.id);
    if (existing != presentations_.end()) {
      if (existing->second.host_id != presentation.host_id ||
          existing->second.session_id != presentation.session_id) {
        throw std::invalid_argument(
            "presentationId cannot move between hosts or sessions");
      }
      presentation.order = existing->second.order;
      OverlayPresentation previous = existing->second;
      existing->second = std::move(presentation);
      try {
        sync();
      } catch (...) {
        existing->second = std::move(previous);
        try {
          sync();
        } catch (...) {
        }
        throw;
      }
    } else {
      presentation.order = next_order_++;
      presentation_order_.push_back(presentation.id);
      const std::string id = presentation.id;
      presentations_.emplace(id, std::move(presentation));
      try {
        sync();
      } catch (...) {
        presentations_.erase(id);
        presentation_order_.pop_back();
        --next_order_;
        try {
          sync();
        } catch (...) {
        }
        throw;
      }
    }
    return true;
  }

  bool remove_image(const std::string& id) {
    require_running();
    if (presentations_.erase(id) == 0) return false;
    presentation_order_.erase(
        std::remove(presentation_order_.begin(), presentation_order_.end(), id),
        presentation_order_.end());
    sync();
    return true;
  }

  bool complete_session(const std::string& session_id) {
    require_running();
    bool removed = false;
    for (auto iterator = presentation_order_.begin();
         iterator != presentation_order_.end();) {
      const auto presentation = presentations_.find(*iterator);
      if (presentation != presentations_.end() &&
          presentation->second.session_id == session_id) {
        presentations_.erase(presentation);
        iterator = presentation_order_.erase(iterator);
        removed = true;
      } else {
        ++iterator;
      }
    }
    suppressed_sessions_.erase(session_id);
    if (active_session_ == session_id) active_session_.clear();
    sync();
    return removed;
  }

  bool invalidate_session(
      const std::string& session_id,
      const std::string& presentation_id) {
    require_running();
    const auto presentation = presentations_.find(presentation_id);
    if (presentation == presentations_.end() ||
        presentation->second.session_id != session_id) {
      return false;
    }
    return remove_image(presentation_id);
  }

  bool suppress_sessions(std::unordered_set<std::string> sessions) {
    require_running();
    suppressed_sessions_ = std::move(sessions);
    sync();
    return true;
  }

  bool set_active_session(const std::string& session_id) {
    require_running();
    active_session_ = session_id;
    sync();
    return true;
  }

  [[nodiscard]] bool has_active() const {
    if (!running_ || !visible_) return false;
    return std::any_of(
        presentations_.begin(), presentations_.end(), [this](const auto& item) {
          return suppressed_sessions_.find(item.second.session_id) ==
                 suppressed_sessions_.end();
        });
  }

  [[nodiscard]] bool has_any() const { return !presentations_.empty(); }

  EventCallback& max_size_callback() { return max_size_callback_; }
  EventCallback& activate_callback() { return activate_callback_; }
  EventCallback& visibility_callback() { return visibility_callback_; }
  void reset_callbacks() {
    max_size_callback_.reset();
    activate_callback_.reset();
    visibility_callback_.reset();
    visibility_dispatcher_.reset();
    relocate_dispatcher_.reset();
    refresh_dispatcher_.reset();
  }

 private:
  void handle_visibility_request(bool visible) {
    if (!running_) return;
    visible_ = visible;
    sync();
    visibility_callback_.emit(visible);
  }

  void require_running() const {
    if (!running_ || !platform_) {
      throw std::runtime_error("overlay.start() must be called first");
    }
  }

  void relocate(const std::string& host_id) {
    const auto host = hosts_.find(host_id);
    if (host == hosts_.end()) return;
    switch (host->second.anchor.edge) {
      case AnchorEdge::kTrailing:
        host->second.anchor.edge = AnchorEdge::kBottom;
        break;
      case AnchorEdge::kBottom:
        host->second.anchor.edge = AnchorEdge::kLeading;
        break;
      case AnchorEdge::kLeading:
        host->second.anchor.edge = AnchorEdge::kTop;
        break;
      case AnchorEdge::kTop:
        host->second.anchor.edge = AnchorEdge::kTrailing;
        break;
    }
    sync();
  }

  void sync() {
    if (!platform_) return;
    OverlaySnapshot snapshot;
    snapshot.options = options_;
    snapshot.visible = visible_;
    snapshot.max_size = max_size_;
    for (const auto& id : host_order_) {
      if (const auto host = hosts_.find(id); host != hosts_.end()) {
        snapshot.hosts.push_back(host->second);
      }
    }

    std::vector<OverlayPresentation> active;
    std::vector<OverlayPresentation> remaining;
    for (const auto& id : presentation_order_) {
      const auto iterator = presentations_.find(id);
      if (iterator == presentations_.end()) continue;
      auto presentation = iterator->second;
      presentation.visible =
          visible_ && suppressed_sessions_.find(presentation.session_id) ==
                          suppressed_sessions_.end();
      if (!active_session_.empty() &&
          presentation.session_id == active_session_) {
        active.push_back(std::move(presentation));
      } else {
        remaining.push_back(std::move(presentation));
      }
    }
    snapshot.presentations.reserve(active.size() + remaining.size());
    snapshot.presentations.insert(
        snapshot.presentations.end(), active.begin(), active.end());
    snapshot.presentations.insert(
        snapshot.presentations.end(), remaining.begin(), remaining.end());
    platform_->update(snapshot);
  }

  bool running_ = false;
  bool visible_ = true;
  double max_size_ = 480;
  std::size_t next_order_ = 0;
  std::string active_session_;
  OverlayOptions options_;
  std::unique_ptr<OverlayPlatform> platform_;
  std::unordered_map<std::string, OverlayHost> hosts_;
  std::vector<std::string> host_order_;
  std::unordered_map<std::string, OverlayPresentation> presentations_;
  std::vector<std::string> presentation_order_;
  std::unordered_set<std::string> suppressed_sessions_;
  EventCallback max_size_callback_;
  EventCallback activate_callback_;
  EventCallback visibility_callback_;
  EventCallback visibility_dispatcher_;
  EventCallback relocate_dispatcher_;
  EventCallback refresh_dispatcher_;
};

OverlayManager manager;

template <typename Function>
Napi::Value invoke(const Napi::CallbackInfo& info, Function function) {
  try {
    return Napi::Boolean::New(info.Env(), function());
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  } catch (const std::exception& error) {
    throw_js_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

Napi::Value start(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    OverlayOptions options;
    if (info.Length() > 0 && info[0].IsObject()) {
      const Napi::Object root = info[0].As<Napi::Object>();
      const Napi::Value tooltip_value = root.Get("tooltip");
      if (tooltip_value.IsObject()) {
        const Napi::Object tooltip = tooltip_value.As<Napi::Object>();
        if (tooltip.Get("hide").IsString()) {
          options.hide_tooltip = tooltip.Get("hide").As<Napi::String>();
        }
        if (tooltip.Get("relocate").IsString()) {
          options.relocate_tooltip =
              tooltip.Get("relocate").As<Napi::String>();
        }
      }
    }
    return manager.start(std::move(options));
  });
}

Napi::Value stop(const Napi::CallbackInfo& info) {
  return invoke(info, [] { return manager.stop(); });
}

Napi::Value attach_host(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsObject()) {
      throw std::invalid_argument("config must be an object");
    }
    const Napi::Object config = info[0].As<Napi::Object>();
    OverlayHost host;
    host.id = required_string(config, "id");
    host.title = required_string(config, "title");
    host.bounds = parse_rect(config.Get("bounds"));
    host.window_handle = parse_window_handle(config.Get("windowHandle"));
    host.anchor = parse_anchor(config.Get("anchor"));
    return manager.attach_host(std::move(host));
  });
}

Napi::Value detach_host(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsString()) {
      throw std::invalid_argument("hostId must be a string");
    }
    return manager.detach_host(info[0].As<Napi::String>());
  });
}

Napi::Value set_visible(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsBoolean()) {
      throw std::invalid_argument("visible must be a boolean");
    }
    return manager.set_visible(info[0].As<Napi::Boolean>());
  });
}

Napi::Value set_max_size(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsNumber()) {
      throw std::invalid_argument("size must be a number");
    }
    return manager.set_max_size(info[0].As<Napi::Number>());
  });
}

Napi::Value push_image(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsObject()) {
      throw std::invalid_argument("frame must be an object");
    }
    const Napi::Object frame = info[0].As<Napi::Object>();
    OverlayPresentation presentation;
    presentation.id = required_string(frame, "presentationId");
    presentation.session_id = required_string(frame, "sessionId");
    presentation.image_data = required_string(frame, "imageData");
    if (frame.Get("hostId").IsString()) {
      presentation.host_id = frame.Get("hostId").As<Napi::String>();
    }
    if (frame.Get("appIconPath").IsString()) {
      presentation.app_icon_path =
          frame.Get("appIconPath").As<Napi::String>().Utf8Value();
    }
    return manager.push_image(std::move(presentation));
  });
}

Napi::Value remove_image(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsString()) {
      throw std::invalid_argument("presentationId must be a string");
    }
    return manager.remove_image(info[0].As<Napi::String>());
  });
}

Napi::Value complete_session(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsString()) {
      throw std::invalid_argument("sessionId must be a string");
    }
    return manager.complete_session(info[0].As<Napi::String>());
  });
}

Napi::Value invalidate_session(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 2 || !info[0].IsString() ||
        !info[1].IsString()) {
      throw std::invalid_argument(
          "sessionId and presentationId must be strings");
    }
    return manager.invalidate_session(
        info[0].As<Napi::String>(), info[1].As<Napi::String>());
  });
}

Napi::Value suppress_sessions(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsArray()) {
      throw std::invalid_argument("sessionIds must be an array");
    }
    const Napi::Array values = info[0].As<Napi::Array>();
    std::unordered_set<std::string> sessions;
    for (std::uint32_t index = 0; index < values.Length(); ++index) {
      const Napi::Value value = values.Get(index);
      if (!value.IsString()) {
        throw std::invalid_argument("sessionIds must contain strings");
      }
      sessions.insert(value.As<Napi::String>());
    }
    return manager.suppress_sessions(std::move(sessions));
  });
}

Napi::Value set_active_session(const Napi::CallbackInfo& info) {
  return invoke(info, [&] {
    if (info.Length() != 1 || !info[0].IsString()) {
      throw std::invalid_argument("sessionId must be a string");
    }
    return manager.set_active_session(info[0].As<Napi::String>());
  });
}

Napi::Value has_active(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), manager.has_active());
}

Napi::Value has_any(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), manager.has_any());
}

template <EventCallback& (OverlayManager::*Getter)()>
Napi::Value set_callback(const Napi::CallbackInfo& info, const char* name) {
  try {
    (manager.*Getter)().set(info, name);
  } catch (const std::invalid_argument& error) {
    throw_js_type_error(info.Env(), error);
  }
  return info.Env().Undefined();
}

}  // namespace

void register_overlay(Napi::Env env, Napi::Object& exports) {
  manager.initialize(env);
  exports.Set("overlayStart", Napi::Function::New(env, start));
  exports.Set("overlayStop", Napi::Function::New(env, stop));
  exports.Set("overlayAttachHost", Napi::Function::New(env, attach_host));
  exports.Set("overlayDetachHost", Napi::Function::New(env, detach_host));
  exports.Set("overlaySetVisible", Napi::Function::New(env, set_visible));
  exports.Set("overlaySetMaxSize", Napi::Function::New(env, set_max_size));
  exports.Set("overlayPushImage", Napi::Function::New(env, push_image));
  exports.Set("overlayRemoveImage", Napi::Function::New(env, remove_image));
  exports.Set(
      "overlayCompleteSession", Napi::Function::New(env, complete_session));
  exports.Set(
      "overlayInvalidateSession", Napi::Function::New(env, invalidate_session));
  exports.Set(
      "overlaySuppressSessions", Napi::Function::New(env, suppress_sessions));
  exports.Set(
      "overlaySetActiveSession", Napi::Function::New(env, set_active_session));
  exports.Set("overlayHasActive", Napi::Function::New(env, has_active));
  exports.Set("overlayHasAny", Napi::Function::New(env, has_any));
  exports.Set(
      "overlayOnMaxSizeChanged",
      Napi::Function::New(
          env,
          [](const Napi::CallbackInfo& info) {
            return set_callback<&OverlayManager::max_size_callback>(
                info, "nativekit.overlay.maxSizeChanged");
          }));
  exports.Set(
      "overlayOnActivate",
      Napi::Function::New(
          env,
          [](const Napi::CallbackInfo& info) {
            return set_callback<&OverlayManager::activate_callback>(
                info, "nativekit.overlay.activate");
          }));
  exports.Set(
      "overlayOnVisibilityRequest",
      Napi::Function::New(
          env,
          [](const Napi::CallbackInfo& info) {
            return set_callback<&OverlayManager::visibility_callback>(
                info, "nativekit.overlay.visibilityRequest");
          }));
}

void cleanup_overlay() noexcept {
  try {
    manager.stop();
  } catch (...) {
  }
  try {
    manager.reset_callbacks();
  } catch (...) {
  }
}

}  // namespace nativekit
