cmake_minimum_required(VERSION 3.22.1)
project(xrgame_native LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(GN "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp" ABSOLUTE)
add_compile_options(-O2 -fno-omit-frame-pointer)
add_link_options(-Wl,--build-id=sha1 -Wl,-z,max-page-size=16384)

option(XRGAME_BUILD_PROBES "Build the debug-only app-domain execution probe" OFF)
if(XRGAME_BUILD_PROBES)
    get_filename_component(XRGAME_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
    if(NOT EXISTS "${XRGAME_ROOT}/foundation/modules/debugbus/CMakeLists.txt")
        message(FATAL_ERROR "Initialize the pinned Foundation submodule: git submodule update --init foundation")
    endif()
    # Internal diagnostics only; the small ring needs no Foundation engine or network module.
    add_subdirectory("${XRGAME_ROOT}/foundation/modules/profiler_ring" foundation-ring EXCLUDE_FROM_ALL)
    add_subdirectory("${XRGAME_ROOT}/foundation/modules/debugbus" foundation-debugbus EXCLUDE_FROM_ALL)
    execute_process(COMMAND git -C "${XRGAME_ROOT}/foundation" rev-parse HEAD
        OUTPUT_VARIABLE FOUNDATION_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    add_library(xrgame_debugbus SHARED debugbus.cpp profiler.cpp)
    target_link_libraries(xrgame_debugbus PRIVATE spatial::foundation_debugbus_profiler_ring dl)
    target_compile_definitions(xrgame_debugbus PRIVATE XRGAME_FOUNDATION_REVISION="${FOUNDATION_REVISION}")
    target_compile_options(xrgame_debugbus PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
    install(TARGETS xrgame_debugbus LIBRARY DESTINATION arm64-v8a)

    add_executable(xrgame_exec_probe exec_probe.c)
    target_compile_options(xrgame_exec_probe PRIVATE -Wall -Wextra -Werror)
    set_target_properties(xrgame_exec_probe PROPERTIES OUTPUT_NAME "libxrgame_exec_probe.so")
    install(TARGETS xrgame_exec_probe RUNTIME DESTINATION arm64-v8a)
endif()

# Build only the libraries used by the bionic/Turnip path. The upstream root
# CMake file references a missing drawable.c and unfinished PatchElf JNI stubs.
add_subdirectory("${GN}/extras/adrenotools" adrenotools)
add_subdirectory("${GN}/lsfg-vk-android/thirdparty/dxbc" dxbc EXCLUDE_FROM_ALL)
add_subdirectory("${GN}/asurfacerenderer" asurfacerenderer)
add_subdirectory("${GN}/xconnectorpatch" xconnectorpatch)

add_library(winlator SHARED
    "${GN}/winlator/sysvshared_memory.c"
    "${GN}/winlator/xconnector_epoll.c"
    "${GN}/winlator/alsa_client.c")
target_link_libraries(winlator log android aaudio)

add_library(winlator_11 SHARED
    "${GN}/asurfacerenderer/drawable.c"
    "${GN}/winlator/gpu_helper.c"
    "${GN}/winlator/wine_registry_editor.c")
target_link_libraries(winlator_11 log android jnigraphics dl m)

add_library(extras SHARED "${GN}/extras/gpu_image.c" "${GN}/extras/vulkan.c")
target_link_libraries(extras log android EGL GLESv2 GLESv3 adrenotools)

add_library(evshim SHARED "${GN}/evshim/evshim.c")
target_compile_definitions(evshim PRIVATE EVSHIM_DEFAULT_BASE_PATH="/data/data/com.tencentmalos.xrgamenative/files")
include(FetchContent)
FetchContent_Declare(sdl_headers
    URL https://github.com/libsdl-org/SDL/archive/5d249570393f7a37e037abf22cd6012a4cc56a71.tar.gz
    URL_HASH SHA256=10f1194f8d2e4a73ca1c7c553b3189d0b68ab9ef3544e8d2a268e4429456b373)
FetchContent_GetProperties(sdl_headers)
if(NOT sdl_headers_POPULATED)
    FetchContent_Populate(sdl_headers)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdl-include")
file(CREATE_LINK "${sdl_headers_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/sdl-include/SDL2" SYMBOLIC)
target_include_directories(evshim PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sdl-include")
target_link_libraries(evshim log dl)

add_library(gamenative_dns_v4mapped SHARED "${GN}/gamenative_dns_v4mapped.c")
target_link_libraries(gamenative_dns_v4mapped dl)

set(SEVENZX_SOURCES 7zAlloc.c 7zArcIn.c 7zBuf.c 7zBuf2.c 7zCrc.c 7zCrcOpt.c 7zDec.c
    7zStream.c Bcj2.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c Lzma2Dec.c LzmaDec.c Ppmd7.c Ppmd7Dec.c)
list(TRANSFORM SEVENZX_SOURCES PREPEND "${GN}/sevenzx/lzma/")
add_executable(sevenzx "${GN}/sevenzx/sevenzx.c" ${SEVENZX_SOURCES})
target_compile_definitions(sevenzx PRIVATE _7ZIP_PPMD_SUPPPORT)
set_target_properties(sevenzx PROPERTIES OUTPUT_NAME "lib7zx.so")
install(TARGETS sevenzx RUNTIME DESTINATION arm64-v8a)

find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
foreach(stage vert frag)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/window_${stage}.h"
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn window_${stage}_code -x
            -o "${CMAKE_CURRENT_BINARY_DIR}/window_${stage}.h" "${GN}/winlator/window.${stage}"
        DEPENDS "${GN}/winlator/window.${stage}" VERBATIM)
endforeach()
set(LSFG_SOURCES vk_dispatch.c lsfg_dll.c lsfg_dxbc.cpp lsfg_probe.c lsfg_jni.c
    lsfg_common.cpp lsfg_shaders.cpp lsfg_mipmaps.cpp lsfg_generate.cpp lsfg_alpha.cpp
    lsfg_beta.cpp lsfg_gamma.cpp lsfg_delta.cpp lsfg_chain.cpp lsfg_pacer.cpp vkr_lsfg.cpp)
list(TRANSFORM LSFG_SOURCES PREPEND "${GN}/lsfg/")
add_library(vulkan_renderer SHARED
    "${GN}/winlator/vulkan_jni.cpp" "${GN}/winlator/VulkanRendererContext.cpp"
    "${GN}/winlator/VulkanRendererScanout.cpp" "${GN}/winlator/VulkanRendererLsfg.cpp"
    ${LSFG_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/window_vert.h" "${CMAKE_CURRENT_BINARY_DIR}/window_frag.h")
target_include_directories(vulkan_renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${GN}/winlator" "${GN}/lsfg")
target_compile_options(vulkan_renderer PRIVATE -fvisibility=hidden)
target_link_libraries(vulkan_renderer log android vulkan adrenotools dxbc dl atomic)

if(DEFINED OpenXR_DIR)
    find_package(OpenXR REQUIRED CONFIG)
    add_subdirectory("${GN}/xrimmersive" xrimmersive)
    install(FILES "$<TARGET_FILE:OpenXR::openxr_loader>" DESTINATION arm64-v8a)
else()
    message(FATAL_ERROR "OpenXR_DIR is required for the complete picoXr native build")
endif()

set(XRGAME_LIBRARIES winlator winlator_11 extras evshim gamenative_dns_v4mapped
    vulkan_renderer asurface_renderer ahbimage drawable xconnectorpatch)
if(TARGET xrimmersive)
    list(APPEND XRGAME_LIBRARIES xrimmersive)
endif()
install(TARGETS ${XRGAME_LIBRARIES} LIBRARY DESTINATION arm64-v8a)
foreach(hook main_hook hook_impl)
    if(TARGET ${hook})
        install(TARGETS ${hook} LIBRARY DESTINATION arm64-v8a)
    endif()
endforeach()
