#import <Foundation/Foundation.h>

#include "ipc/secure_channel.h"

#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/frame_decoder.h"

extern char** environ;

namespace nativekit::platform {
namespace {

std::string canonical_executable(const std::string& path) {
  std::array<char, PATH_MAX> canonical{};
  if (realpath(path.c_str(), canonical.data()) == nullptr) return {};
  struct stat status {};
  if (stat(canonical.data(), &status) != 0 || !S_ISREG(status.st_mode) ||
      access(canonical.data(), X_OK) != 0) {
    return {};
  }
  return canonical.data();
}

class MacSecureChannelPlatform final : public SecureChannelPlatform {
 public:
  explicit MacSecureChannelPlatform(SecureChannelEvents events)
      : events_(std::move(events)) {}

  ~MacSecureChannelPlatform() override { stop(); }

  std::optional<std::uint32_t> spawn(
      const std::string& executable_path,
      const std::vector<std::string>& arguments) override {
    std::lock_guard operation_lock(operation_mutex_);
    reap_threads();
    {
      std::lock_guard lock(mutex_);
      if (pid_ > 0) return std::nullopt;
    }

    const std::string canonical = canonical_executable(executable_path);
    if (canonical.empty()) return std::nullopt;

    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0) return std::nullopt;
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    const int null_descriptor = open("/dev/null", O_RDWR);
    if (null_descriptor < 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      return std::nullopt;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    posix_spawn_file_actions_adddup2(
        &actions, null_descriptor, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(
        &actions, null_descriptor, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    posix_spawn_file_actions_addclose(&actions, null_descriptor);

    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_START_SUSPENDED |
                  POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child = 0;
    std::vector<std::string> argument_storage;
    argument_storage.reserve(arguments.size() + 1);
    argument_storage.push_back(canonical);
    argument_storage.insert(
        argument_storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argument_values;
    argument_values.reserve(argument_storage.size() + 1);
    for (auto& argument : argument_storage) {
      argument_values.push_back(argument.data());
    }
    argument_values.push_back(nullptr);
    const int result = posix_spawn(
        &child,
        canonical.c_str(),
        &actions,
        &attributes,
        argument_values.data(),
        environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    close(null_descriptor);
    if (result != 0) {
      close(descriptors[0]);
      return std::nullopt;
    }

    if (!verify(static_cast<std::uint32_t>(child), canonical)) {
      kill(-child, SIGKILL);
      waitpid(child, nullptr, 0);
      close(descriptors[0]);
      return std::nullopt;
    }

    {
      std::lock_guard lock(mutex_);
      pid_ = child;
      read_descriptor_ = descriptors[0];
      channel_error_.store(false);
    }
    try {
      reader_thread_ =
          std::thread([this, descriptor = descriptors[0], child] {
            read_frames(descriptor, child);
          });
      wait_thread_ = std::thread([this, child] { wait_for_exit(child); });
    } catch (...) {
      stop_worker();
      return std::nullopt;
    }
    if (kill(-child, SIGCONT) != 0) {
      stop_worker();
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(child);
  }

  bool verify(
      std::uint32_t pid,
      const std::string& executable_path) const override {
    const std::string expected = canonical_executable(executable_path);
    if (expected.empty()) return false;
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> actual{};
    const int length = proc_pidpath(
        static_cast<int>(pid), actual.data(), static_cast<std::uint32_t>(actual.size()));
    if (length <= 0) return false;
    const std::string actual_path = canonical_executable(actual.data());
    return !actual_path.empty() && actual_path == expected;
  }

  bool active() const override {
    std::lock_guard lock(mutex_);
    return pid_ > 0;
  }

  void stop() override {
    std::lock_guard operation_lock(operation_mutex_);
    stop_worker();
  }

 private:
  void stop_worker() {
    pid_t child = 0;
    {
      std::lock_guard lock(mutex_);
      child = pid_;
    }
    if (child > 0) kill(-child, SIGKILL);
    join_threads();
    std::lock_guard lock(mutex_);
    pid_ = 0;
    read_descriptor_ = -1;
  }

  void reap_threads() {
    bool active = false;
    {
      std::lock_guard lock(mutex_);
      active = pid_ > 0;
    }
    if (!active) join_threads();
  }

  void join_threads() {
    if (wait_thread_.joinable() &&
        wait_thread_.get_id() != std::this_thread::get_id()) {
      wait_thread_.join();
    }
    join_reader_thread();
  }

  void join_reader_thread() {
    if (reader_thread_.joinable() &&
        reader_thread_.get_id() != std::this_thread::get_id()) {
      reader_thread_.join();
    }
  }

  void read_frames(int descriptor, pid_t child) {
    FrameDecoder decoder;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    try {
      while (true) {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
          if (errno == EINTR) continue;
          channel_error_.store(true);
          kill(-child, SIGKILL);
          break;
        }
        for (auto& frame : decoder.push(buffer.data(), count)) {
          if (events_.emit) {
            events_.emit({
                SecureChannelEventType::kData,
                std::move(frame),
                0,
            });
          }
        }
      }
    } catch (...) {
      channel_error_.store(true);
      kill(-child, SIGKILL);
    }
    if (decoder.has_pending_data()) {
      channel_error_.store(true);
      kill(-child, SIGKILL);
    }
    close(descriptor);
    std::lock_guard lock(mutex_);
    if (read_descriptor_ == descriptor) read_descriptor_ = -1;
  }

  void wait_for_exit(pid_t child) {
    int status = 0;
    pid_t waited = 0;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    kill(-child, SIGKILL);
    join_reader_thread();
    std::int32_t code = -1;
    if (waited == child && WIFEXITED(status)) {
      code = WEXITSTATUS(status);
    } else if (waited == child && WIFSIGNALED(status)) {
      code = 128 + WTERMSIG(status);
    }
    if (channel_error_.load()) code = -1;
    if (events_.emit) {
      events_.emit({SecureChannelEventType::kExit, {}, code});
    }
    {
      std::lock_guard lock(mutex_);
      if (pid_ == child) pid_ = 0;
    }
  }

  SecureChannelEvents events_;
  mutable std::mutex operation_mutex_;
  mutable std::mutex mutex_;
  pid_t pid_ = 0;
  int read_descriptor_ = -1;
  std::atomic<bool> channel_error_ = false;
  std::thread reader_thread_;
  std::thread wait_thread_;
};

}  // namespace

std::unique_ptr<SecureChannelPlatform> create_secure_channel_platform(
    SecureChannelEvents events) {
  return std::make_unique<MacSecureChannelPlatform>(std::move(events));
}

}  // namespace nativekit::platform
