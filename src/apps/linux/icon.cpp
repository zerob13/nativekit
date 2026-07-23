#include "apps/icon.h"

#include "common/linux/image_utils.h"

namespace nativekit::platform {

std::optional<std::string> app_icon(
    const std::string& app_path,
    int pixels) {
  return icon_to_png_data_url(app_path, pixels);
}

}  // namespace nativekit::platform
