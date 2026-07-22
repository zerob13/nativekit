#include "drag/drag_source.h"

#include <windows.h>

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nativekit::platform {
namespace {

std::wstring utf8_to_utf16(const std::string& value) {
  if (value.empty() || value.find('\0') != std::string::npos ||
      value.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (length <= 0) return {};

  std::wstring converted(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          converted.data(),
          length) != length) {
    return {};
  }
  return converted;
}

bool client_coordinate(double value, double scale, LONG* result) {
  if (!std::isfinite(value)) return false;
  const double rounded = std::round(value * scale);
  if (rounded < std::numeric_limits<LONG>::min() ||
      rounded > std::numeric_limits<LONG>::max()) {
    return false;
  }
  *result = static_cast<LONG>(rounded);
  return true;
}

struct PreparedRequest {
  std::vector<std::wstring> files;
  HWND window = nullptr;
  POINT client_position{};
  POINT origin{};
};

bool prepare_request(const DragRequest& request, PreparedRequest* prepared) {
  const HWND window = reinterpret_cast<HWND>(request.window_handle);
  DWORD process_id = 0;
  if (!IsWindow(window) ||
      GetWindowThreadProcessId(window, &process_id) == 0 ||
      process_id != GetCurrentProcessId()) {
    return false;
  }

  const UINT dpi = GetDpiForWindow(window);
  const double scale = static_cast<double>(dpi == 0 ? 96 : dpi) / 96.0;
  POINT client{};
  if (!client_coordinate(request.position.x, scale, &client.x) ||
      !client_coordinate(request.position.y, scale, &client.y)) {
    return false;
  }
  const POINT client_position = client;
  RECT client_bounds{};
  if (!GetClientRect(window, &client_bounds) ||
      !PtInRect(&client_bounds, client) || !ClientToScreen(window, &client)) {
    return false;
  }

  if (request.files.empty()) return false;
  prepared->files.reserve(request.files.size());
  for (const auto& file : request.files) {
    std::wstring path = utf8_to_utf16(file);
    if (path.empty() || !std::filesystem::path(path).is_absolute()) {
      return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    prepared->files.push_back(std::move(path));
  }
  prepared->window = window;
  prepared->client_position = client_position;
  prepared->origin = client;
  return true;
}

class FileDataObject final : public IDataObject {
 public:
  explicit FileDataObject(std::vector<std::wstring> files)
      : files_(std::move(files)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(
      REFIID interface_id,
      void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (interface_id == __uuidof(IUnknown) ||
        interface_id == __uuidof(IDataObject)) {
      *object = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetData(
      FORMATETC* format,
      STGMEDIUM* medium) override {
    if (format == nullptr || medium == nullptr) return E_POINTER;
    const HRESULT supported = QueryGetData(format);
    if (FAILED(supported)) return supported;

    std::size_t character_count = 1;
    for (const auto& file : files_) {
      if (file.size() >
          (std::numeric_limits<std::size_t>::max() - character_count - 1)) {
        return E_OUTOFMEMORY;
      }
      character_count += file.size() + 1;
    }
    if (character_count >
        (std::numeric_limits<std::size_t>::max() - sizeof(DROPFILES)) /
            sizeof(wchar_t)) {
      return E_OUTOFMEMORY;
    }
    const std::size_t byte_count =
        sizeof(DROPFILES) + character_count * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byte_count);
    if (memory == nullptr) return E_OUTOFMEMORY;

    void* locked = GlobalLock(memory);
    if (locked == nullptr) {
      GlobalFree(memory);
      return E_OUTOFMEMORY;
    }
    auto* drop_files = static_cast<DROPFILES*>(locked);
    drop_files->pFiles = sizeof(DROPFILES);
    drop_files->fWide = TRUE;
    auto* destination = reinterpret_cast<wchar_t*>(
        static_cast<std::byte*>(locked) + sizeof(DROPFILES));
    for (const auto& file : files_) {
      std::memcpy(
          destination, file.data(), file.size() * sizeof(wchar_t));
      destination += file.size();
      *destination++ = L'\0';
    }
    *destination = L'\0';
    GlobalUnlock(memory);

    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(
      FORMATETC*,
      STGMEDIUM*) override {
    return DATA_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (format == nullptr) return E_POINTER;
    if (format->cfFormat != CF_HDROP) return DV_E_FORMATETC;
    if ((format->tymed & TYMED_HGLOBAL) == 0) return DV_E_TYMED;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (format->lindex != -1) return DV_E_LINDEX;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
      FORMATETC*,
      FORMATETC* canonical) override {
    if (canonical == nullptr) return E_POINTER;
    canonical->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
  }

  HRESULT STDMETHODCALLTYPE SetData(
      FORMATETC*,
      STGMEDIUM*,
      BOOL) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnumFormatEtc(
      DWORD direction,
      IEnumFORMATETC** formats) override {
    if (formats == nullptr) return E_POINTER;
    *formats = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    FORMATETC format{
        static_cast<CLIPFORMAT>(CF_HDROP),
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL,
    };
    return SHCreateStdEnumFmtEtc(1, &format, formats);
  }

  HRESULT STDMETHODCALLTYPE DAdvise(
      FORMATETC*,
      DWORD,
      IAdviseSink*,
      DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

 private:
  ~FileDataObject() = default;

  std::atomic<ULONG> references_{1};
  std::vector<std::wstring> files_;
};

class FileDropSource final : public IDropSource {
 public:
  explicit FileDropSource(const std::atomic_bool& cancelled)
      : cancelled_(cancelled) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(
      REFIID interface_id,
      void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (interface_id == __uuidof(IUnknown) ||
        interface_id == __uuidof(IDropSource)) {
      *object = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE QueryContinueDrag(
      BOOL escape_pressed,
      DWORD key_state) override {
    if (cancelled_.load(std::memory_order_relaxed) || escape_pressed) {
      return DRAGDROP_S_CANCEL;
    }
    if ((key_state & (MK_LBUTTON | MK_RBUTTON)) == 0) {
      return DRAGDROP_S_DROP;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

 private:
  ~FileDropSource() = default;

  std::atomic<ULONG> references_{1};
  const std::atomic_bool& cancelled_;
};

class WindowsDragPlatform final : public DragPlatform {
 public:
  explicit WindowsDragPlatform(DragEvents events)
      : events_(std::move(events)) {}

  ~WindowsDragPlatform() override { stop(); }

  bool start(const DragRequest& request) override {
    PreparedRequest prepared;
    if (!prepare_request(request, &prepared)) return false;

    std::thread completed;
    {
      std::lock_guard lock(mutex_);
      if (active_) return false;
      if (worker_.joinable()) completed = std::move(worker_);
    }
    if (completed.joinable()) completed.join();

    std::promise<bool> initialized;
    std::future<bool> ready = initialized.get_future();
    {
      std::lock_guard lock(mutex_);
      if (active_) return false;
      cancelled_.store(false, std::memory_order_relaxed);
      active_ = true;
      try {
        worker_ = std::thread(
            [this,
             prepared = std::move(prepared),
             initialized = std::move(initialized)]() mutable {
              run(std::move(prepared), std::move(initialized));
            });
      } catch (...) {
        active_ = false;
        throw;
      }
    }

    if (ready.get()) return true;

    std::thread failed;
    {
      std::lock_guard lock(mutex_);
      if (worker_.joinable()) failed = std::move(worker_);
    }
    if (failed.joinable()) failed.join();
    return false;
  }

  void stop() override {
    std::thread worker;
    DWORD thread_id = 0;
    {
      std::lock_guard lock(mutex_);
      cancelled_.store(true, std::memory_order_relaxed);
      thread_id = thread_id_;
      if (worker_.joinable()) worker = std::move(worker_);
    }
    if (thread_id != 0) {
      PostThreadMessageW(thread_id, WM_KEYDOWN, VK_ESCAPE, 0);
      PostThreadMessageW(thread_id, WM_KEYUP, VK_ESCAPE, 0);
      PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    }
    if (worker.joinable()) worker.join();
  }

 private:
  void run(PreparedRequest prepared, std::promise<bool> initialized) {
    const HRESULT ole_result = OleInitialize(nullptr);
    if (FAILED(ole_result)) {
      {
        std::lock_guard lock(mutex_);
        active_ = false;
        thread_id_ = 0;
      }
      initialized.set_value(false);
      return;
    }

    FileDataObject* data_object =
        new (std::nothrow) FileDataObject(std::move(prepared.files));
    FileDropSource* drop_source =
        new (std::nothrow) FileDropSource(cancelled_);
    if (data_object == nullptr || drop_source == nullptr) {
      if (data_object != nullptr) data_object->Release();
      if (drop_source != nullptr) drop_source->Release();
      OleUninitialize();
      {
        std::lock_guard lock(mutex_);
        active_ = false;
        thread_id_ = 0;
      }
      initialized.set_value(false);
      return;
    }

    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
      std::lock_guard lock(mutex_);
      thread_id_ = GetCurrentThreadId();
    }
    initialized.set_value(true);

    IDragSourceHelper* drag_helper = nullptr;
    if (SUCCEEDED(CoCreateInstance(
            CLSID_DragDropHelper,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&drag_helper)))) {
      drag_helper->InitializeFromWindow(
          prepared.window, &prepared.client_position, data_object);
      drag_helper->Release();
    }

    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(
        data_object,
        drop_source,
        DROPEFFECT_COPY,
        &effect);
    drop_source->Release();
    data_object->Release();

    POINT final_position = prepared.origin;
    GetCursorPos(&final_position);
    OleUninitialize();
    {
      std::lock_guard lock(mutex_);
      active_ = false;
      thread_id_ = 0;
    }

    if (events_.ended) {
      events_.ended({
          result == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY) != 0,
          {static_cast<double>(final_position.x),
           static_cast<double>(final_position.y)},
      });
    }
  }

  DragEvents events_;
  std::mutex mutex_;
  std::thread worker_;
  std::atomic_bool cancelled_{false};
  bool active_ = false;
  DWORD thread_id_ = 0;
};

}  // namespace

std::unique_ptr<DragPlatform> create_drag_platform(DragEvents events) {
  return std::make_unique<WindowsDragPlatform>(std::move(events));
}

}  // namespace nativekit::platform
