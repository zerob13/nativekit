#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <optional>
#include <string>

namespace nativekit::platform {

// The caller owns the returned reference.
GdkPixbuf* pixbuf_from_data_url(const std::string& data_url);

// The caller owns the returned reference.
GdkPixbuf* app_icon_pixbuf(const std::string& app_path, int pixels);

std::optional<std::string> pixbuf_to_png_data_url(
    GdkPixbuf* pixbuf,
    int pixels);

std::optional<std::string> icon_to_png_data_url(
    const std::string& app_path,
    int pixels);

}  // namespace nativekit::platform
