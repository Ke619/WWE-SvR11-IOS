# WWE SmackDown vs. Raw 2011 on Nintendo Switch (Horizon, libnx, GCC).
# See port/docs/SWITCH_PORT_PLAN.md. Configure with devkitPro's Switch.cmake from
# MSYS2 (tools: port/switch/tools); paths must have no spaces for its tools, so the
# SDK, plume and generated code are private copies in /work/nx.
cmake_minimum_required(VERSION 3.25)
project(svr2011_switch LANGUAGES C CXX ASM)

set(SVR_PORT "/work/nx/port" CACHE PATH "port folder (a link without spaces: the tools need that)")
set(REXSDK_DIR "/work/nx/rex" CACHE PATH "the SDK, with the svr2011 and Switch patches")
set(PLUME_DIR "/work/nx/plume" CACHE PATH "plume, with the svr2011 patch")
set(SVR_GENERATED "/work/nx/generated" CACHE PATH "generated recompiled code")
set(NVK_SDK "/work/nvk-sdk" CACHE PATH "mesa-switch: libvulkan.a and its parts")
option(SVR_SDK_ONLY "Only the SDK's libraries" OFF)
option(SVR_NX_REPORTS_LIVE "Release builds: Report a problem may use the public server (test builds never do)" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_HOST_TOOL "/bin/true" CACHE FILEPATH "" FORCE)
add_subdirectory(${REXSDK_DIR} rexglue-sdk)

if(SVR_SDK_ONLY)
  return()
endif()

# What the SDK's own folders are built with (its settings don't reach this one).
add_compile_definitions(REX_PLATFORM_SWITCH=1 REX_PLATFORM_LINUX=1 NDEBUG REXGLUE_BUILD_CONFIG="Release")
# (-fPIE after CMake's -fPIC: nothing here is interposed, so calls can inline and own data needs no table.)
add_compile_options(-fno-strict-aliasing -w -fPIE -fno-semantic-interposition
  "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${REXSDK_DIR}/compat_nx/rex_nx_compat.h>")
include_directories(BEFORE SYSTEM ${REXSDK_DIR}/compat_nx)
get_filename_component(SVR_GENERATED_PARENT "${SVR_GENERATED}" DIRECTORY)

# ---- recompiled game code ----
set(CMAKE_CURRENT_SOURCE_DIR_SAVED "${CMAKE_CURRENT_SOURCE_DIR}")
file(GLOB SVR_RECOMP CONFIGURE_DEPENDS "${SVR_GENERATED}/default/*.cpp")
add_library(svr2011_recomp OBJECT ${SVR_RECOMP})
target_include_directories(svr2011_recomp PRIVATE ${SVR_GENERATED} ${SVR_GENERATED}/default ${SVR_PORT}/src)
target_link_libraries(svr2011_recomp PRIVATE rex::runtime)
target_precompile_headers(svr2011_recomp PRIVATE "${SVR_GENERATED}/default/svr2011_pch.h")
target_compile_options(svr2011_recomp PRIVATE -O2 -g0 -fno-math-errno -fno-trapping-math)

# ---- the game's own code: the same list as port/CMakeLists.txt ----
file(READ "${SVR_PORT}/CMakeLists.txt" SVR_TOP)
string(REGEX MATCH "set\\(SVR2011_SOURCES[^)]*\\)" SVR_LIST "${SVR_TOP}")
string(REGEX MATCHALL "(src|modmaker/svrfmt)/[A-Za-z0-9_/]+\\.cpp" SVR_FILES "${SVR_LIST}")
list(APPEND SVR_FILES
  src/model_mem.cpp src/mem_trace.cpp
  src/native/gpu.cpp src/native/backend_vulkan.cpp src/native/recording_list.cpp
  src/native/native_renderer.cpp src/native/native_hooks.cpp src/native/textures.cpp
  src/native/texture_packs.cpp
  src/posix_stubs.cpp)
set(SVR_SOURCES "")
foreach(f ${SVR_FILES})
  list(APPEND SVR_SOURCES "${SVR_PORT}/${f}")
endforeach()
list(APPEND SVR_SOURCES
  ${REXSDK_DIR}/src/graphics/pipeline/texture/util.cpp
  ${REXSDK_DIR}/src/graphics/pipeline/texture/info_formats.cpp
  ${REXSDK_DIR}/src/ui/windowed_app_main_sdl.cpp
  ${REXSDK_DIR}/src/ui/rex_app.cpp
  ${SVR_PORT}/launcher/bink_decode.c
  ${SVR_PORT}/launcher/bink_encode.c
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_stubs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_profile.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_ab.cpp ${CMAKE_CURRENT_SOURCE_DIR}/nx_fast.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nx_report.cpp ${CMAKE_CURRENT_SOURCE_DIR}/nx_install_check.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sdl3_nx/sdl_nx.cpp
  ${PLUME_DIR}/plume_vulkan.cpp)

file(STRINGS "${SVR_PORT}/VERSION.txt" SVR_VERSION LIMIT_COUNT 1)
add_executable(svr2011 ${SVR_SOURCES} $<TARGET_OBJECTS:svr2011_recomp>)
target_include_directories(svr2011 PRIVATE
  ${SVR_PORT} ${SVR_PORT}/src ${SVR_PORT}/launcher ${SVR_PORT}/modmaker/svrfmt ${SVR_GENERATED_PARENT} ${SVR_GENERATED} ${SVR_GENERATED}/default ${PLUME_DIR}
  ${PLUME_DIR}/contrib ${PLUME_DIR}/contrib/volk ${PLUME_DIR}/contrib/Vulkan-Headers/include
  ${PLUME_DIR}/contrib/VulkanMemoryAllocator/include
  $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
  ${REXSDK_DIR}/thirdparty/xxHash ${REXSDK_DIR}/thirdparty/stb)
target_compile_options(svr2011 PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${SVR_PORT}/src/compat.h>")
target_compile_definitions(svr2011 PRIVATE
  SVR2011_PORT_VERSION="${SVR_VERSION}" SVR2011_BUILD_COMMIT="switch"
  PLUME_SWITCH=1)
if(SVR_NX_REPORTS_LIVE)
  target_compile_definitions(svr2011 PRIVATE SVR_NX_REPORTS_LIVE=1)
endif()
# (nx_report.cpp: zlib and Mbed TLS, from devkitPro's portlibs)
find_path(NX_PORTLIBS_INCLUDE mbedtls/ssl.h REQUIRED)
target_include_directories(svr2011 PRIVATE ${NX_PORTLIBS_INCLUDE})
target_compile_options(svr2011 PRIVATE -O2)

target_link_directories(svr2011 PRIVATE ${NVK_SDK}/lib)
target_link_libraries(svr2011 PRIVATE rex::runtime rexgpu-xenos
  -Wl,--allow-multiple-definition -Wl,--wrap=svcBreak -Wl,--wrap=stat -Wl,--wrap=lstat -Wl,--wrap=_open_r -Wl,--wrap=_read_r -Wl,--wrap=_lseek_r -Wl,--wrap=_close_r -Wl,--wrap=_write_r -Wl,--wrap=rename -Wl,--wrap=fchmod -Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=closedir -Wl,--wrap=pthread_detach -Wl,--wrap=threadStart -Wl,--start-group
  vulkan nvk nak nak_rs nil libnil_format_table nouveau_horizon nouveau_mme nouveau_ws nvidia_headers_c
  vtn nir compiler compiler_c_helpers vulkan_util mesa_util mesa_util_c11 mesa_util_simd xmlconfig blake3
  expat zstd mbedtls mbedx509 mbedcrypto z nx stdc++ m -Wl,--end-group)
