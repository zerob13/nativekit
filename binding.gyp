{
  "targets": [
    {
      "target_name": "nativekit",
      "cflags_cc": ["-std=c++17", "-fexceptions"],
      "defines": ["NAPI_VERSION=8", "NODE_ADDON_API_CPP_EXCEPTIONS"],
      "include_dirs": ["<!@(node -p \"require('node-addon-api').include_dir\")"],
      "conditions": [
        ["OS=='mac'", {
          "sources": [
            "src/binding.cpp",
            "src/common/types.cc",
            "src/overlay/overlay_manager.cc",
            "src/overlay/mac/overlay_window.mm",
            "src/overlay/mac/overlay_stack.mm",
            "src/overlay/mac/overlay_controls.mm",
            "src/windows/window_query.cc",
            "src/windows/mac/window_query.mm",
            "src/ipc/secure_channel.cc",
            "src/ipc/mac/xpc_channel.mm",
            "src/apps/icon.cc",
            "src/apps/mac/icon.mm",
            "src/drag/drag_source.cc",
            "src/drag/mac/drag_source.mm"
          ],
          "link_settings": {
            "libraries": [
              "-framework AppKit",
              "-framework CoreGraphics",
              "-framework CoreFoundation"
            ],
          },
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
          }
        }],
        ["OS=='win'", {
          "sources": [
            "src/binding.cpp",
            "src/common/types.cc",
            "src/overlay/overlay_manager.cc",
            "src/overlay/win/overlay_window.cpp",
            "src/overlay/win/overlay_stack.cpp",
            "src/windows/window_query.cc",
            "src/windows/win/window_query.cpp",
            "src/ipc/secure_channel.cc",
            "src/ipc/win/pipe_channel.cpp",
            "src/apps/icon.cc",
            "src/apps/win/icon.cpp",
            "src/drag/drag_source.cc",
            "src/drag/win/drag_source.cpp"
          ],
          "libraries": [
            "dwmapi.lib", "shlwapi.lib", "shell32.lib",
            "ole32.lib", "oleaut32.lib", "uuid.lib",
            "user32.lib", "gdi32.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "LanguageStandard": "stdcpp17",
              "ExceptionHandling": "1"
            }
          }
        }]
      ]
    }
  ]
}
