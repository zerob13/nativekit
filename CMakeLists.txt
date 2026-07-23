cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

project(nativekit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(NATIVEKIT_SOURCES
  src/binding.cpp
  src/apps/icon.cc
  src/common/event_callback.cc
  src/overlay/overlay_manager.cc
  src/windows/window_query.cc
)

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

  list(APPEND NATIVEKIT_SOURCES
    src/apps/mac/icon.mm
    src/common/mac/image_utils.mm
    src/overlay/mac/overlay_window.mm
    src/windows/mac/window_query.mm
  )

  find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
  find_library(CORE_GRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  find_library(QUARTZ_CORE_FRAMEWORK QuartzCore REQUIRED)
endif()

if(WIN32)
  list(APPEND NATIVEKIT_SOURCES
    src/apps/win/icon.cpp
    src/common/win/image_utils.cpp
    src/overlay/win/overlay_window.cpp
    src/windows/win/window_query.cpp
  )
endif()

if(UNIX AND NOT APPLE)
  list(APPEND NATIVEKIT_SOURCES
    src/apps/linux/icon.cpp
    src/common/linux/image_utils.cpp
    src/overlay/linux/overlay_window.cpp
    src/windows/linux/window_query.cpp
  )

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(
    LINUX_DEPS
    REQUIRED
    IMPORTED_TARGET
    gdk-pixbuf-2.0
    gio-unix-2.0
    xcb
    xcb-randr
  )
endif()

execute_process(
  COMMAND node -p "require('node-addon-api').include_dir"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)

if(MSVC AND CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
  execute_process(
    COMMAND ${CMAKE_AR}
      /def:${CMAKE_JS_NODELIB_DEF}
      /out:${CMAKE_JS_NODELIB_TARGET}
      ${CMAKE_STATIC_LINKER_FLAGS}
    COMMAND_ERROR_IS_FATAL ANY
  )
endif()

add_library(nativekit SHARED ${NATIVEKIT_SOURCES} ${CMAKE_JS_SRC})

target_include_directories(nativekit PRIVATE
  "${NODE_ADDON_API_DIR}"
  "${CMAKE_SOURCE_DIR}/src"
  ${CMAKE_JS_INC}
)

target_compile_definitions(nativekit PRIVATE
  NAPI_VERSION=8
  NODE_ADDON_API_CPP_EXCEPTIONS
)

target_link_libraries(nativekit PRIVATE ${CMAKE_JS_LIB})

if(APPLE)
  target_compile_options(nativekit PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(nativekit PRIVATE
    ${APPKIT_FRAMEWORK}
    ${CORE_GRAPHICS_FRAMEWORK}
    ${QUARTZ_CORE_FRAMEWORK}
  )
  target_link_options(nativekit PRIVATE "-undefined" "dynamic_lookup")
endif()

if(WIN32)
  set_property(
    TARGET nativekit
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  )
  target_compile_definitions(nativekit PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
  )
  target_link_libraries(nativekit PRIVATE
    comctl32
    crypt32
    dwmapi
    gdi32
    ole32
    shell32
    user32
    windowscodecs
  )
endif()

if(UNIX AND NOT APPLE)
  target_compile_definitions(nativekit PRIVATE
    GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_56
  )
  target_link_libraries(nativekit PRIVATE PkgConfig::LINUX_DEPS)
endif()

set_target_properties(nativekit PROPERTIES PREFIX "" SUFFIX ".node")
