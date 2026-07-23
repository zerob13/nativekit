{
  "targets": [
    {
      "target_name": "nativekit",
      "sources": [
        "src/binding.cpp",
        "src/apps/icon.cc",
        "src/common/event_callback.cc",
        "src/overlay/overlay_manager.cc",
        "src/windows/window_query.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "src"
      ],
      "defines": [
        "NAPI_VERSION=8",
        "NODE_ADDON_API_CPP_EXCEPTIONS"
      ],
      "cflags_cc": ["-std=c++17", "-fexceptions"],
      "conditions": [
        ["OS=='mac'", {
          "sources": [
            "src/apps/mac/icon.mm",
            "src/common/mac/image_utils.mm",
            "src/overlay/mac/overlay_window.mm",
            "src/windows/mac/window_query.mm"
          ],
          "link_settings": {
            "libraries": [
              "-framework AppKit",
              "-framework CoreGraphics",
              "-framework QuartzCore"
            ]
          },
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "CLANG_ENABLE_OBJC_ARC": "YES",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "12.0"
          }
        }],
        ["OS=='win'", {
          "sources": [
            "src/apps/win/icon.cpp",
            "src/common/win/image_utils.cpp",
            "src/overlay/win/overlay_window.cpp",
            "src/windows/win/window_query.cpp"
          ],
          "defines": [
            "NOMINMAX",
            "UNICODE",
            "_UNICODE",
            "_WIN32_WINNT=0x0A00",
            "WIN32_LEAN_AND_MEAN"
          ],
          "libraries": [
            "comctl32.lib",
            "crypt32.lib",
            "dwmapi.lib",
            "gdi32.lib",
            "ole32.lib",
            "shell32.lib",
            "user32.lib",
            "windowscodecs.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "LanguageStandard": "stdcpp17"
            }
          }
        }],
        ["OS=='linux'", {
          "sources": [
            "src/apps/linux/icon.cpp",
            "src/common/linux/image_utils.cpp",
            "src/overlay/linux/overlay_window.cpp",
            "src/windows/linux/window_query.cpp"
          ],
          "cflags": [
            "<!@(pkg-config --cflags gdk-pixbuf-2.0 gio-unix-2.0 xcb xcb-randr)"
          ],
          "defines": [
            "GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_56"
          ],
          "libraries": [
            "<!@(pkg-config --libs gdk-pixbuf-2.0 gio-unix-2.0 xcb xcb-randr)"
          ]
        }]
      ]
    }
  ]
}
