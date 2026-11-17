cmake_minimum_required(VERSION 3.24)
project(voidvr LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "VOID/BREAKER is 64-bit: configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(OPENXR_BUILD ${TP}/build-openxr CACHE PATH "OpenXR-SDK build directory")

add_library(minhook STATIC
    ${TP}/minhook/src/buffer.c
    ${TP}/minhook/src/hook.c
    ${TP}/minhook/src/trampoline.c
    ${TP}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

# The mod is a version.dll proxy: the game loads it from Binaries\Win64.
add_library(version SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/log.cpp
    src/proxy_stubs.asm
    src/config.cpp
    src/hooks.cpp
    src/ue5.cpp
    src/stereo_device.cpp
    src/render.cpp
    src/vr.cpp
    src/xr.cpp
    src/xr_input.cpp
    src/gamepad.cpp
    src/weapon.cpp
    src/version.def)
target_include_directories(version PRIVATE ${TP}/OpenXR-SDK/include)
target_compile_definitions(version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D12)
target_compile_options(version PRIVATE /W3 /permissive- /Zi /EHsc)
target_link_options(version PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(version PRIVATE minhook
    ${OPENXR_BUILD}/src/loader/$<IF:$<CONFIG:Debug>,Debug/openxr_loaderd.lib,Release/openxr_loader.lib>
    dxguid)
