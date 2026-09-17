# Compile the libyue static library from a source distribution, with the
# same configuration as the MoonBit bindings consume (C++20, /MT on Windows),
# so users of the bindings do not have to compile libyue locally.
#
# Usage:
#   cmake -S scripts/prebuilt -B out/Prebuilt/<arch> \
#         -DDIST_DIR=<path to out/Dist/source> -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.13)

# Target macOS version, has to be set before creating project.
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
elseif(WIN32)
  # Use Windows 10 SDK; must be set before project().
  set(CMAKE_SYSTEM_VERSION 10.0)
  # Match the /MT static CRT used by the moon toolchain.
  cmake_policy(SET CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

project(libyue_prebuilt CXX C)

set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../out/Dist/source"
    CACHE PATH "Path to the extracted libyue source distribution")

# ---------- libyue platform sources ----------
if(APPLE)
  # The ASM language is required for the .S units (e.g. CallWithEHFrame),
  # otherwise CMake silently skips them and linking fails.
  enable_language(ASM)
  file(GLOB_RECURSE YUE_SOURCES
       ${DIST_DIR}/src/mac/*.cc ${DIST_DIR}/src/mac/*.mm
       ${DIST_DIR}/src/mac/*.S ${DIST_DIR}/src/mac/*.c)
  set(YUE_NO_ARC_SOURCES ${YUE_SOURCES})
  list(FILTER YUE_SOURCES EXCLUDE REGEX "allocator|nativeui")
  list(FILTER YUE_NO_ARC_SOURCES INCLUDE REGEX "allocator|nativeui")
elseif(WIN32)
  file(GLOB_RECURSE YUE_SOURCES
       ${DIST_DIR}/src/win/*.cc ${DIST_DIR}/src/win/*.c)
else()
  file(GLOB_RECURSE YUE_SOURCES
       ${DIST_DIR}/src/linux/*.cc ${DIST_DIR}/src/linux/*.c)
endif()

add_library(libyue STATIC ${YUE_SOURCES})
set_target_properties(libyue PROPERTIES OUTPUT_NAME yue)

target_include_directories(libyue PUBLIC
  ${DIST_DIR}/include
  ${DIST_DIR}/include/base/allocator/partition_allocator/src)

target_compile_features(libyue PUBLIC cxx_std_20)

# ---------- macOS ----------
if(APPLE)
  find_library(APPKIT AppKit)
  find_library(CARBON Carbon)
  find_library(IOKIT IOKit)
  find_library(SECURITY Security)
  find_library(WEBKIT WebKit)
  find_library(OD OpenDirectory)
  target_link_libraries(libyue PUBLIC
    ${APPKIT} ${CARBON} ${IOKIT} ${SECURITY} ${WEBKIT} ${OD})
  target_compile_definitions(libyue PUBLIC SYSTEM_NATIVE_UTF8)
  target_compile_options(libyue PRIVATE
    -Wno-deprecated-declarations -Wno-unguarded-availability-new)
  # Compile files that disable ARC as a separate library.
  add_library(libyue_noarc STATIC ${YUE_NO_ARC_SOURCES})
  target_include_directories(libyue_noarc PUBLIC
    ${DIST_DIR}/include
    ${DIST_DIR}/include/base/allocator/partition_allocator/src)
  target_compile_features(libyue_noarc PUBLIC cxx_std_20)
  set_property(TARGET libyue APPEND PROPERTY COMPILE_OPTIONS
               -fobjc-arc -fno-objc-arc-exceptions)
endif()

# ---------- Windows ----------
if(WIN32)
  target_compile_definitions(libyue PUBLIC
    _WINDOWS WIN32 WIN32_LEAN_AND_MEAN NOMINMAX _UNICODE UNICODE
    # base::StackContainer requires it under Debug (upstream comment).
    $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
  target_link_libraries(libyue PUBLIC
    user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib
    comdlg32.lib imm32.lib msimg32.lib oleacc.lib usp10.lib
    setupapi.lib powrprof.lib ws2_32.lib dbghelp.lib shlwapi.lib version.lib
    winmm.lib wbemuuid.lib psapi.lib dwmapi.lib propsys.lib comctl32.lib
    gdiplus.lib urlmon.lib userenv.lib uxtheme.lib delayimp.lib
    runtimeobject.lib ntdll.lib shcore.lib pdh.lib)
  target_compile_options(libyue PRIVATE /wd4005 /wd4101 /wd4244 /wd4267 /wd4996)
  set_target_properties(libyue PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR})
  # Browser support needs the WebView2 SDK headers, which are bundled into
  # the distribution under include/third_party (any version).
  file(GLOB WEBVIEW2_SDK_DIRS
       ${DIST_DIR}/include/third_party/Microsoft.Web.WebView2.*)
  if(NOT WEBVIEW2_SDK_DIRS STREQUAL "")
    target_compile_definitions(libyue PUBLIC WEBVIEW2_SUPPORT)
    target_include_directories(libyue PUBLIC
      ${WEBVIEW2_SDK_DIRS}/build/native/include)
  endif()
endif()

# ---------- Linux ----------
if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_search_module(FONTCONFIG REQUIRED fontconfig)
  pkg_search_module(PANGO REQUIRED pangoft2)
  pkg_search_module(GTK3 REQUIRED gtk+-3.0)
  pkg_search_module(X11 REQUIRED x11)
  # Ubuntu 22.04 and earlier ship webkit2gtk-4.0; 24.04 onwards only 4.1.
  pkg_search_module(WEBKIT2GTK REQUIRED webkit2gtk-4.0 webkit2gtk-4.1)
  target_include_directories(libyue PUBLIC
    ${FONTCONFIG_INCLUDE_DIRS} ${PANGO_INCLUDE_DIRS} ${GTK3_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS} ${WEBKIT2GTK_INCLUDE_DIRS})
  target_compile_options(libyue PRIVATE
    -Wno-deprecated-declarations -Wno-unknown-attributes)
  target_compile_definitions(libyue PUBLIC USE_GLIB)
endif()
