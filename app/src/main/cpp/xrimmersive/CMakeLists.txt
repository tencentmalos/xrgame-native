# Native OpenXR session for the Meta Quest "immersive launch" mode.
#
# Built against the official Khronos OpenXR loader (org.khronos.openxr:openxr_loader_for_android,
# Apache-2.0, pulled in as a Gradle/prefab dependency in app/build.gradle.kts) — no code or
# binaries from any third-party Winlator VR fork are used here.
#
# NOT wired into the normal Gradle build (see the other commented-out externalNativeBuild
# blocks in app/build.gradle.kts) — this project ships native libs as prebuilt .so files
# committed to jniLibs/, rebuilt manually when needed. To build this module locally:
#   1. Temporarily uncomment the externalNativeBuild { cmake { path = ... } } block for
#      this CMakeLists.txt in app/build.gradle.kts.
#   2. Build, then copy the resulting libxrimmersive.so into
#      app/src/main/jniLibs/arm64-v8a/ and re-comment the block.
cmake_minimum_required(VERSION 3.22.1)
project(xrimmersive)

add_library(xrimmersive SHARED
    xr_immersive.cpp
    xr_capture.cpp
    xr_perf_hud.cpp
    xr_keyboard.cpp
    xr_launch_panel.cpp
    xr_imgui_fonts.cpp
    xr_imgui_style.cpp
    xr_pinyin.cpp
    xr_frame_generation.cpp
    jni_bridge.cpp
    sbs_jni.cpp
    xr_vulkan.cpp
    xr_vulkan_compositor.cpp
    xr_vulkan_projection.cpp
    xr_vulkan_probe.cpp
    xr_vulkan_trace.cpp
    xr_windows_projection.cpp
    xr_windows_transport.cpp
)

find_package(OpenXR REQUIRED CONFIG)

# The Vulkan composite needs the Foundation reconstruction, HUD and frame generation targets and adrenotools, which
# the picoXr native build (app/src/picoXr/cpp/CMakeLists.txt) defines before this directory.
if(NOT TARGET xrgame_foundation_xr OR NOT TARGET xrgame_foundation_hud OR NOT TARGET adrenotools OR
   NOT TARGET spatial::foundation_lossless_fg)
    message(FATAL_ERROR "xrimmersive is built by the picoXr native build (xrgame_foundation_xr, xrgame_foundation_hud, foundation_lossless_fg, adrenotools)")
endif()

target_link_libraries(xrimmersive PRIVATE
    xrgame_foundation_xr
    xrgame_foundation_hud
    spatial::foundation_lossless_fg
    adrenotools
    OpenXR::openxr_loader
    EGL
    GLESv3
    android
    jnigraphics
    log
)

# Debug builds with the scrcpy capture SDK (app/src/picoXr/cpp/CMakeLists.txt) record and screenshot
# the session; without it the capture commands answer unavailable.
if(TARGET spatial::foundation_capture)
    target_link_libraries(xrimmersive PRIVATE spatial::foundation_capture)
    target_compile_definitions(xrimmersive PRIVATE XRGAME_CAPTURE=1)
endif()

# Pinyin for the XR keyboard: AOSP PinyinIME's engine (libgooglepinyin, Apache-2.0) from
# app/src/main/cpp/extras/pinyinime. Without it the keyboard stays Latin.
set(XRGAME_PINYIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../extras/pinyinime/jni" CACHE PATH "AOSP PinyinIME engine")
if(EXISTS "${XRGAME_PINYIN_DIR}/share/pinyinime.cpp")
    file(GLOB XRGAME_PINYIN_SOURCES "${XRGAME_PINYIN_DIR}/share/*.cpp")
    add_library(xrgame_pinyin STATIC ${XRGAME_PINYIN_SOURCES})
    target_include_directories(xrgame_pinyin PUBLIC "${XRGAME_PINYIN_DIR}/include")
    # Upstream code from 2009; its warnings are not ours to fix here.
    target_compile_options(xrgame_pinyin PRIVATE -w)
    target_link_libraries(xrimmersive PRIVATE xrgame_pinyin)
    target_compile_definitions(xrimmersive PRIVATE XRGAME_PINYIN=1)
else()
    message(WARNING "No PinyinIME engine at ${XRGAME_PINYIN_DIR}: the XR keyboard has no pinyin")
endif()

target_compile_definitions(xrimmersive PRIVATE
    XR_USE_PLATFORM_ANDROID=1
    XR_USE_GRAPHICS_API_OPENGL_ES=1
    XR_USE_GRAPHICS_API_VULKAN=1
    EGL_EGLEXT_PROTOTYPES=1
    GL_GLEXT_PROTOTYPES=1
)
target_compile_features(xrimmersive PRIVATE cxx_std_20)

target_compile_options(xrimmersive PRIVATE -Wall -Wextra -Werror=return-type -Wno-missing-field-initializers)
