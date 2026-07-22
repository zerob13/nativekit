#include "common/win/image_utils.h"

#include <windows.h>

#include <ShlObj.h>
#include <Wincrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

namespace nativekit::platform {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment {
 public:
  ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComApartment() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }
  [[nodiscard]] bool available() const {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_;
};

std::optional<std::string> stream_to_data_url(IStream* stream) {
  HGLOBAL memory = nullptr;
  if (FAILED(GetHGlobalFromStream(stream, &memory)) || memory == nullptr) {
    return std::nullopt;
  }
  const SIZE_T size = GlobalSize(memory);
  const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
  if (bytes == nullptr || size == 0) return std::nullopt;

  DWORD encoded_size = 0;
  constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
  CryptBinaryToStringA(
      bytes, static_cast<DWORD>(size), flags, nullptr, &encoded_size);
  std::string encoded(encoded_size, '\0');
  const BOOL encoded_ok = CryptBinaryToStringA(
      bytes,
      static_cast<DWORD>(size),
      flags,
      encoded.data(),
      &encoded_size);
  GlobalUnlock(memory);
  if (!encoded_ok) return std::nullopt;
  if (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
  return "data:image/png;base64," + encoded;
}

}  // namespace

std::optional<std::string> icon_to_png_data_url(
    const std::wstring& path,
    int pixels) {
  if (path.empty() || pixels <= 0) return std::nullopt;
  ComApartment apartment;
  if (!apartment.available()) return std::nullopt;

  SHFILEINFOW file_info{};
  const UINT icon_flag = pixels <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON;
  if (SHGetFileInfoW(
          path.c_str(),
          0,
          &file_info,
          sizeof(file_info),
          SHGFI_ICON | icon_flag) == 0 ||
      file_info.hIcon == nullptr) {
    return std::nullopt;
  }

  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;

  HRESULT result = CoCreateInstance(
      CLSID_WICImagingFactory,
      nullptr,
      CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&factory));
  if (SUCCEEDED(result)) result = factory->CreateBitmapFromHICON(file_info.hIcon, &bitmap);
  if (SUCCEEDED(result)) result = factory->CreateBitmapScaler(&scaler);
  if (SUCCEEDED(result)) {
    result = scaler->Initialize(
        bitmap.Get(),
        static_cast<UINT>(pixels),
        static_cast<UINT>(pixels),
        WICBitmapInterpolationModeFant);
  }
  if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(result)) {
    result = converter->Initialize(
        scaler.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0,
        WICBitmapPaletteTypeCustom);
  }
  if (SUCCEEDED(result)) result = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (SUCCEEDED(result)) {
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, &properties);
  if (SUCCEEDED(result)) result = frame->Initialize(properties.Get());

  if (SUCCEEDED(result)) {
    result = frame->SetSize(
        static_cast<UINT>(pixels), static_cast<UINT>(pixels));
  }
  WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixel_format);
  if (SUCCEEDED(result)) result = frame->WriteSource(converter.Get(), nullptr);
  if (SUCCEEDED(result)) result = frame->Commit();
  if (SUCCEEDED(result)) result = encoder->Commit();

  DestroyIcon(file_info.hIcon);
  return SUCCEEDED(result) ? stream_to_data_url(stream.Get()) : std::nullopt;
}

}  // namespace nativekit::platform
