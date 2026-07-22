#include "ipc/secure_channel.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/frame_decoder.h"

namespace nativekit::platform {
namespace {

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  ~Handle() { reset(); }

  bool valid() const {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  HANDLE get() const { return value_; }
  HANDLE release() {
    const HANDLE value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(HANDLE value = nullptr) {
    if (valid()) CloseHandle(value_);
    value_ = value;
  }

 private:
  HANDLE value_ = nullptr;
};

class AttributeList {
 public:
  AttributeList() = default;
  AttributeList(const AttributeList&) = delete;
  AttributeList& operator=(const AttributeList&) = delete;
  ~AttributeList() {
    if (list_ != nullptr) DeleteProcThreadAttributeList(list_);
    if (memory_ != nullptr) HeapFree(GetProcessHeap(), 0, memory_);
  }

  bool initialize(HANDLE* handles, std::size_t count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) return false;
    memory_ = HeapAlloc(GetProcessHeap(), 0, size);
    if (memory_ == nullptr) return false;
    list_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory_);
    if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
      list_ = nullptr;
      return false;
    }
    return UpdateProcThreadAttribute(
               list_,
               0,
               PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
               handles,
               sizeof(HANDLE) * count,
               nullptr,
               nullptr) != FALSE;
  }

  LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

 private:
  void* memory_ = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

std::wstring utf8_to_utf16(const std::string& value) {
  if (value.empty()) return {};
  if (value.find('\0') != std::string::npos ||
      value.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int size = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (size <= 0) return {};
  std::wstring converted(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          converted.data(),
          size) != size) {
    return {};
  }
  return converted;
}

std::wstring canonical_executable(const std::wstring& path) {
  if (path.empty()) return {};
  const DWORD full_size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (full_size == 0) return {};
  std::wstring full_path(full_size, L'\0');
  const DWORD full_length = GetFullPathNameW(
      path.c_str(), full_size, full_path.data(), nullptr);
  if (full_length == 0 || full_length >= full_size) return {};
  full_path.resize(full_length);

  const DWORD attributes = GetFileAttributesW(full_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return {};
  }

  Handle file(CreateFileW(
      full_path.c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!file.valid() || GetFileType(file.get()) != FILE_TYPE_DISK) return {};

  constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
  const DWORD final_size =
      GetFinalPathNameByHandleW(file.get(), nullptr, 0, flags);
  if (final_size == 0) return {};
  std::wstring canonical(final_size, L'\0');
  const DWORD final_length = GetFinalPathNameByHandleW(
      file.get(), canonical.data(), final_size, flags);
  if (final_length == 0 || final_length >= final_size) return {};
  canonical.resize(final_length);
  return canonical;
}

std::wstring canonical_executable(const std::string& path) {
  return canonical_executable(utf8_to_utf16(path));
}

bool same_path(const std::wstring& left, const std::wstring& right) {
  if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      right.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool verify_process(HANDLE process, const std::wstring& expected) {
  std::wstring path(32768, L'\0');
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &length) ||
      length == 0) {
    return false;
  }
  path.resize(length);
  const std::wstring actual = canonical_executable(path);
  return !actual.empty() && same_path(actual, expected);
}

void append_quoted_argument(std::wstring& command_line, const std::wstring& arg) {
  command_line.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : arg) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      command_line.append(backslashes * 2 + 1, L'\\');
      command_line.push_back(L'"');
    } else {
      command_line.append(backslashes, L'\\');
      command_line.push_back(character);
    }
    backslashes = 0;
  }
  command_line.append(backslashes * 2, L'\\');
  command_line.push_back(L'"');
}

std::optional<std::wstring> make_command_line(
    const std::wstring& executable,
    const std::vector<std::string>& arguments) {
  std::wstring command_line;
  append_quoted_argument(command_line, executable);
  for (const auto& argument : arguments) {
    const std::wstring converted = utf8_to_utf16(argument);
    if (!argument.empty() && converted.empty()) return std::nullopt;
    command_line.push_back(L' ');
    append_quoted_argument(command_line, converted);
  }
  if (command_line.size() >= 32767) return std::nullopt;
  return command_line;
}

class WindowsSecureChannelPlatform final : public SecureChannelPlatform {
 public:
  explicit WindowsSecureChannelPlatform(SecureChannelEvents events)
      : events_(std::move(events)) {}

  ~WindowsSecureChannelPlatform() override { stop(); }

  std::optional<std::uint32_t> spawn(
      const std::string& executable_path,
      const std::vector<std::string>& arguments) override {
    std::lock_guard operation_lock(operation_mutex_);
    if (!prepare_for_spawn()) return std::nullopt;

    const std::wstring executable = canonical_executable(executable_path);
    if (executable.empty()) return std::nullopt;
    auto command_line = make_command_line(executable, arguments);
    if (!command_line) return std::nullopt;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE raw_read_pipe = nullptr;
    HANDLE raw_write_pipe = nullptr;
    if (!CreatePipe(&raw_read_pipe, &raw_write_pipe, &security, 0)) {
      return std::nullopt;
    }
    Handle read_pipe(raw_read_pipe);
    Handle write_pipe(raw_write_pipe);
    if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0)) {
      return std::nullopt;
    }

    Handle null_input(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    Handle null_error(CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!null_input.valid() || !null_error.valid()) return std::nullopt;

    std::array<HANDLE, 3> inherited_handles = {
        write_pipe.get(), null_input.get(), null_error.get()};
    AttributeList attributes;
    if (!attributes.initialize(
            inherited_handles.data(), inherited_handles.size())) {
      return std::nullopt;
    }

    Handle current_token;
    HANDLE raw_current_token = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY,
            &raw_current_token)) {
      return std::nullopt;
    }
    current_token.reset(raw_current_token);

    Handle restricted_token;
    HANDLE raw_restricted_token = nullptr;
    if (!CreateRestrictedToken(
            current_token.get(),
            DISABLE_MAX_PRIVILEGE,
            0,
            nullptr,
            0,
            nullptr,
            0,
            nullptr,
            &raw_restricted_token)) {
      return std::nullopt;
    }
    restricted_token.reset(raw_restricted_token);

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) return std::nullopt;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &job_limits,
            sizeof(job_limits))) {
      return std::nullopt;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.get();
    startup.StartupInfo.hStdOutput = write_pipe.get();
    startup.StartupInfo.hStdError = null_error.get();
    startup.lpAttributeList = attributes.get();

    PROCESS_INFORMATION process_info{};
    const DWORD creation_flags =
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessAsUserW(
            restricted_token.get(),
            executable.c_str(),
            command_line->data(),
            nullptr,
            nullptr,
            TRUE,
            creation_flags,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process_info)) {
      return std::nullopt;
    }

    Handle process(process_info.hProcess);
    Handle primary_thread(process_info.hThread);
    write_pipe.reset();
    null_input.reset();
    null_error.reset();

    if (!AssignProcessToJobObject(job.get(), process.get())) {
      TerminateProcess(process.get(), ERROR_ACCESS_DENIED);
      WaitForSingleObject(process.get(), INFINITE);
      return std::nullopt;
    }
    if (!verify_process(process.get(), executable)) {
      TerminateJobObject(job.get(), ERROR_INVALID_IMAGE_HASH);
      WaitForSingleObject(process.get(), INFINITE);
      return std::nullopt;
    }

    const DWORD child_pid = process_info.dwProcessId;
    const HANDLE child_process = process.get();
    const HANDLE child_pipe = read_pipe.get();
    {
      std::lock_guard lock(state_mutex_);
      pid_ = child_pid;
      process_ = process.release();
      job_ = job.release();
      read_pipe_ = read_pipe.release();
      channel_error_.store(false);
    }

    try {
      reader_thread_ = std::thread(
          [this, child_pipe, child_pid] {
            read_frames(child_pipe, child_pid);
          });
      wait_thread_ = std::thread(
          [this, child_process, child_pid] {
            wait_for_exit(child_process, child_pid);
          });
    } catch (...) {
      stop_worker();
      return std::nullopt;
    }

    if (ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1)) {
      stop_worker();
      return std::nullopt;
    }
    return child_pid;
  }

  bool verify(
      std::uint32_t pid,
      const std::string& executable_path) const override {
    if (pid == 0) return false;
    const std::wstring expected = canonical_executable(executable_path);
    if (expected.empty()) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return process.valid() && verify_process(process.get(), expected);
  }

  bool active() const override {
    std::lock_guard lock(state_mutex_);
    return pid_ != 0;
  }

  void stop() override {
    std::lock_guard operation_lock(operation_mutex_);
    stop_worker();
  }

 private:
  bool prepare_for_spawn() {
    HANDLE process = nullptr;
    bool has_worker = false;
    {
      std::lock_guard lock(state_mutex_);
      process = process_;
      has_worker = process_ != nullptr || job_ != nullptr ||
                   read_pipe_ != nullptr || reader_thread_.joinable() ||
                   wait_thread_.joinable();
    }
    if (!has_worker) return true;
    if (process == nullptr ||
        WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
      return false;
    }
    stop_worker();
    return true;
  }

  void stop_worker() {
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    {
      std::lock_guard lock(state_mutex_);
      pid_ = 0;
      job = job_;
      job_ = nullptr;
      process = process_;
    }

    if (job != nullptr) {
      TerminateJobObject(job, ERROR_PROCESS_ABORTED);
      CloseHandle(job);
    } else if (process != nullptr &&
               WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      TerminateProcess(process, ERROR_PROCESS_ABORTED);
    }

    join_threads();

    std::lock_guard lock(state_mutex_);
    if (read_pipe_ != nullptr) {
      CloseHandle(read_pipe_);
      read_pipe_ = nullptr;
    }
    if (process_ != nullptr) {
      CloseHandle(process_);
      process_ = nullptr;
    }
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

  void terminate_for_channel_error(std::uint32_t child_pid) {
    channel_error_.store(true);
    std::lock_guard lock(state_mutex_);
    if (pid_ == child_pid && job_ != nullptr) {
      TerminateJobObject(job_, ERROR_INVALID_DATA);
    }
  }

  void read_frames(HANDLE pipe, std::uint32_t child_pid) {
    FrameDecoder decoder;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    try {
      while (true) {
        DWORD count = 0;
        if (!ReadFile(
                pipe,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &count,
                nullptr)) {
          if (GetLastError() != ERROR_BROKEN_PIPE) {
            terminate_for_channel_error(child_pid);
          }
          break;
        }
        if (count == 0) break;
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
      terminate_for_channel_error(child_pid);
    }
    if (decoder.has_pending_data()) {
      terminate_for_channel_error(child_pid);
    }

    CloseHandle(pipe);
    std::lock_guard lock(state_mutex_);
    if (read_pipe_ == pipe) read_pipe_ = nullptr;
  }

  void wait_for_exit(HANDLE process, std::uint32_t child_pid) {
    std::int32_t exit_code = -1;
    if (WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0) {
      DWORD native_exit_code = 0;
      if (GetExitCodeProcess(process, &native_exit_code)) {
        exit_code = static_cast<std::int32_t>(native_exit_code);
      }
    }
    {
      std::lock_guard lock(state_mutex_);
      if (pid_ == child_pid && job_ != nullptr) {
        TerminateJobObject(job_, ERROR_PROCESS_ABORTED);
      }
    }
    join_reader_thread();
    if (channel_error_.load()) exit_code = -1;
    if (events_.emit) {
      events_.emit({SecureChannelEventType::kExit, {}, exit_code});
    }
    {
      std::lock_guard lock(state_mutex_);
      if (pid_ == child_pid) pid_ = 0;
    }
  }

  SecureChannelEvents events_;
  mutable std::mutex operation_mutex_;
  mutable std::mutex state_mutex_;
  std::uint32_t pid_ = 0;
  HANDLE process_ = nullptr;
  HANDLE job_ = nullptr;
  HANDLE read_pipe_ = nullptr;
  std::atomic<bool> channel_error_ = false;
  std::thread reader_thread_;
  std::thread wait_thread_;
};

}  // namespace

std::unique_ptr<SecureChannelPlatform> create_secure_channel_platform(
    SecureChannelEvents events) {
  return std::make_unique<WindowsSecureChannelPlatform>(std::move(events));
}

}  // namespace nativekit::platform
