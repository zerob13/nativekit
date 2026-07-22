{
  "targets": [
    {
      "target_name": "nativekit",
      "sources": [
        "src/binding.cpp",
        "src/apps/icon.cc",
        "src/common/event_callback.cc",
        "src/drag/drag_source.cc",
        "src/ipc/secure_channel.cc",
        "src/overlay/overlay_manager.cc",
        "src/windows/window_query.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include_dir\")",
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
            "src/drag/mac/drag_source.mm",
            "src/ipc/mac/secure_channel.mm",
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
            "src/drag/win/drag_source.cpp",
            "src/ipc/win/secure_channel.cpp",
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
            "advapi32.lib",
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
        }]
      ]
    }
  ]
}
