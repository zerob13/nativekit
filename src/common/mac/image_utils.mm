#import "common/mac/image_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace {

constexpr std::size_t kMaximumDataUrlLength = 32 * 1024 * 1024;
constexpr NSInteger kMaximumImageDimension = 8192;
constexpr std::uint64_t kMaximumPixelCount = (64 * 1024 * 1024) / 4;

}  // namespace

namespace nativekit::platform {

std::optional<std::string> image_to_png_data_url(
    NSImage* image,
    double point_size) {
  if (image == nil || point_size <= 0) return std::nullopt;

  const NSInteger pixels = static_cast<NSInteger>(point_size);
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes:nullptr
                    pixelsWide:pixels
                    pixelsHigh:pixels
                 bitsPerSample:8
               samplesPerPixel:4
                      hasAlpha:YES
                      isPlanar:NO
                colorSpaceName:NSCalibratedRGBColorSpace
                   bytesPerRow:0
                  bitsPerPixel:0];
  if (bitmap == nil) return std::nullopt;

  [NSGraphicsContext saveGraphicsState];
  NSGraphicsContext.currentContext =
      [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
  [image drawInRect:NSMakeRect(0, 0, pixels, pixels)
           fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver
           fraction:1.0
     respectFlipped:YES
              hints:@{NSImageHintInterpolation : @(NSImageInterpolationHigh)}];
  [NSGraphicsContext restoreGraphicsState];

  NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
  if (png == nil) return std::nullopt;
  NSString* encoded = [png base64EncodedStringWithOptions:0];
  return "data:image/png;base64," + std::string(encoded.UTF8String);
}

NSImage* image_from_data_url(const std::string& data_url) {
  if (data_url.empty() || data_url.size() > kMaximumDataUrlLength) return nil;
  const std::size_t separator = data_url.find(',');
  if (separator == std::string::npos || separator + 1 >= data_url.size()) {
    return nil;
  }
  std::string header = data_url.substr(0, separator);
  std::transform(
      header.begin(),
      header.end(),
      header.begin(),
      [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (header != "data:image/png;base64" &&
      header != "data:image/jpeg;base64" &&
      header != "data:image/jpg;base64") {
    return nil;
  }
  NSString* encoded = [NSString
      stringWithUTF8String:data_url.substr(separator + 1).c_str()];
  NSData* data = [[NSData alloc] initWithBase64EncodedString:encoded options:0];
  if (data == nil) return nil;
  NSImage* image = [[NSImage alloc] initWithData:data];
  if (image == nil || image.representations.count == 0) return nil;
  for (NSImageRep* representation in image.representations) {
    const NSInteger width = representation.pixelsWide;
    const NSInteger height = representation.pixelsHigh;
    if (width <= 0 || height <= 0 || width > kMaximumImageDimension ||
        height > kMaximumImageDimension ||
        static_cast<std::uint64_t>(width) * height > kMaximumPixelCount) {
      return nil;
    }
  }
  return image;
}

}  // namespace nativekit::platform
