cmake_minimum_required(VERSION 3.25)
project(ShadeTube VERSION 0.6.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: single exe
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "ShadeTube is a Windows application (Direct2D / WASAPI / Media Foundation).")
endif()

# ---- YoutubeExplode (the user's C++ port) ----------------------------------------------------
set(YTE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(YTE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YTE_HTTP_BACKEND "WINHTTP" CACHE STRING "" FORCE)
add_subdirectory(external/YoutubeExplode EXCLUDE_FROM_ALL)

# ---- Common compile settings for our own targets ----------------------------------------------
add_library(st_options INTERFACE)
target_compile_options(st_options INTERFACE /W4 /utf-8 /permissive- /Zc:__cplusplus /bigobj /MP
    /wd4100   # unreferenced parameter (virtual overrides)
    /wd4324)  # structure padded due to alignment specifier
target_compile_definitions(st_options INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000B)
target_include_directories(st_options INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/YoutubeExplode/third_party)   # nlohmann/json
# Release: whole-program optimisation, small & fast.
target_compile_options(st_options INTERFACE $<$<CONFIG:Release,RelWithDebInfo>:/GL /Gy /Gw /O2>)
target_link_options(st_options INTERFACE $<$<CONFIG:Release,RelWithDebInfo>:/LTCG /OPT:REF /OPT:ICF>)

# WebView2 (Spotify sp_dc login window). Downloads the SDK at configure time and links the STATIC loader so
# the app stays a single self-contained exe. Needs the Evergreen WebView2 Runtime at run time (Win 11 ships it).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WebView2.cmake)

add_subdirectory(src/core)
add_subdirectory(src/catalog)
add_subdirectory(src/musicbrainz)
add_subdirectory(src/spotify)
add_subdirectory(src/youtube)
add_subdirectory(src/audio)
add_subdirectory(src/lyrics)
add_subdirectory(src/player)
add_subdirectory(src/gfx)
add_subdirectory(src/ui)
add_subdirectory(src/app)

option(ST_BUILD_TESTS "Build developer test programs" ON)
if(ST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
