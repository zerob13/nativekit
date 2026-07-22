cmake_minimum_required(VERSION 3.22)
project(nativekit LANGUAGES C CXX)

# --- C++ standard ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- macOS: enable Objective-C++ / Swift interop ---
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  # Link AppKit / CoreGraphics frameworks
  find_library(APPKIT AppKit)
  find_library(CORE_GRAPHICS CoreGraphics)
  find_library(CORE_FOUNDATION CoreFoundation)
endif()

# --- Windows: link system libs ---
if(WIN32)
  set(SYSTEM_LIBS dwmapi shlwapi shell32 ole32 oleaut32 uuid user32 gdi32)
endif()

# --- Sources ---
set(NATIVEKIT_SOURCES
  src/binding.cpp
  src/common/types.cc
  # overlay
  src/overlay/overlay_manager.cc
  # windows
  src/windows/window_query.cc
  # ipc
  src/ipc/secure_channel.cc
  # apps
  src/apps/icon.cc
  # drag
  src/drag/drag_source.cc
)

if(APPLE)
  list(APPEND NATIVEKIT_SOURCES
    src/overlay/mac/overlay_window.mm
    src/overlay/mac/overlay_stack.mm
    src/overlay/mac/overlay_controls.mm
    src/windows/mac/window_query.mm
    src/ipc/mac/xpc_channel.mm
    src/apps/mac/icon.mm
    src/drag/mac/drag_source.mm
  )
endif()

if(WIN32)
  list(APPEND NATIVEKIT_SOURCES
    src/overlay/win/overlay_window.cpp
    src/overlay/win/overlay_stack.cpp
    src/windows/win/window_query.cpp
    src/ipc/win/pipe_channel.cpp
    src/apps/win/icon.cpp
    src/drag/win/drag_source.cpp
  )
endif()

# --- node-addon-api ---
execute_process(
  COMMAND node -p "require('node-addon-api').include_dir"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_library(nativekit SHARED ${NATIVEKIT_SOURCES})

target_include_directories(nativekit PRIVATE
  ${NODE_ADDON_API_DIR}
  ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(nativekit PRIVATE
  NAPI_VERSION=8
  NODE_ADDON_API_CPP_EXCEPTIONS
)

if(APPLE)
  target_link_libraries(nativekit PRIVATE
    ${APPKIT}
    ${CORE_GRAPHICS}
    ${CORE_FOUNDATION}
  )
  set_target_properties(nativekit PROPERTIES
    LINK_FLAGS "-undefined dynamic_lookup"
  )
endif()

if(WIN32)
  target_link_libraries(nativekit PRIVATE ${SYSTEM_LIBS})
endif()

# cmake-js output naming
set_target_properties(nativekit PROPERTIES
  PREFIX ""
  SUFFIX ".node"
)
