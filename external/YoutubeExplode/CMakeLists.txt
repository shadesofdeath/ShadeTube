cmake_minimum_required(VERSION 3.16)

project(YoutubeExplode
    VERSION 1.0.0
    DESCRIPTION "C++ port of YoutubeExplode: YouTube metadata, stream extraction (with signature deciphering), captions, playlists, channels and search"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(YTE_BUILD_EXAMPLES "Build the example console program" ON)
option(YTE_BUILD_TESTS "Build the offline unit tests" ON)

# HTTP backend: WinHTTP (native, no dependencies) on Windows, libcurl elsewhere.
if(WIN32)
    set(YTE_DEFAULT_HTTP_BACKEND "WINHTTP")
else()
    set(YTE_DEFAULT_HTTP_BACKEND "CURL")
endif()
set(YTE_HTTP_BACKEND "${YTE_DEFAULT_HTTP_BACKEND}" CACHE STRING "HTTP backend: WINHTTP or CURL")
set_property(CACHE YTE_HTTP_BACKEND PROPERTY STRINGS WINHTTP CURL)

add_library(YoutubeExplode STATIC
    src/ClientContext.cpp
    src/Common.cpp
    src/Ids.cpp
    src/YoutubeClient.cpp
    src/Bridge/BrowseResponses.cpp
    src/Bridge/Common.cpp
    src/Bridge/Manifests.cpp
    src/Bridge/Pages.cpp
    src/Bridge/PlayerResponse.cpp
    src/Bridge/PlayerSource.cpp
    src/Bridge/Cipher/CipherManifest.cpp
    src/Bridge/Cipher/JsScanner.cpp
    src/Channels/ChannelClient.cpp
    src/Http/CurlHttpClient.cpp
    src/Http/HttpClient.cpp
    src/Http/WinHttpClient.cpp
    src/Playlists/PlaylistClient.cpp
    src/Music/TrackMatcher.cpp
    src/Search/SearchClient.cpp
    src/Utils/Crypto.cpp
    src/Utils/Json.cpp
    src/Utils/StringUtils.cpp
    src/Utils/Time.cpp
    src/Utils/Url.cpp
    src/Utils/Xml.cpp
    src/Videos/VideoClient.cpp
    src/Videos/VideoController.cpp
    src/Videos/ClosedCaptions/ClosedCaptions.cpp
    src/Videos/Streams/MediaStream.cpp
    src/Videos/Streams/StreamClient.cpp
    src/Videos/Streams/StreamPrimitives.cpp
)
add_library(YoutubeExplode::YoutubeExplode ALIAS YoutubeExplode)

target_include_directories(YoutubeExplode
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

target_compile_features(YoutubeExplode PUBLIC cxx_std_17)

if(YTE_HTTP_BACKEND STREQUAL "WINHTTP")
    if(NOT WIN32)
        message(FATAL_ERROR "The WINHTTP backend is only available on Windows.")
    endif()
    target_compile_definitions(YoutubeExplode PRIVATE YTE_HTTP_WINHTTP)
    target_link_libraries(YoutubeExplode PUBLIC winhttp)
elseif(YTE_HTTP_BACKEND STREQUAL "CURL")
    find_package(CURL REQUIRED)
    target_compile_definitions(YoutubeExplode PRIVATE YTE_HTTP_CURL)
    target_link_libraries(YoutubeExplode PUBLIC CURL::libcurl)
else()
    message(FATAL_ERROR "Unknown YTE_HTTP_BACKEND '${YTE_HTTP_BACKEND}' (expected WINHTTP or CURL).")
endif()

if(MSVC)
    # /utf-8: sources contain UTF-8; /bigobj: nlohmann/json; /Zc:__cplusplus for correct feature detection.
    target_compile_options(YoutubeExplode PRIVATE /W4 /utf-8 /bigobj /Zc:__cplusplus /permissive-)
    target_compile_definitions(YoutubeExplode PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(YoutubeExplode PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(YTE_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if(YTE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS YoutubeExplode EXPORT YoutubeExplodeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT YoutubeExplodeTargets NAMESPACE YoutubeExplode:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/YoutubeExplode)
