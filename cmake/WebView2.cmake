# Fetches the Microsoft.Web.WebView2 SDK (a NuGet package is just a zip) at configure time and exposes a
# STATIC IMPORTED target `webview2`. We link the *static* loader (WebView2LoaderStatic.lib) so the app stays
# a single self-contained exe — no WebView2Loader.dll to ship. The actual browser is the Evergreen WebView2
# Runtime, which is present on Windows 11 (and installable via the bootstrapper on Windows 10).
set(WEBVIEW2_VERSION "1.0.2792.45" CACHE STRING "Microsoft.Web.WebView2 SDK version")

set(_wv2_dir    "${CMAKE_BINARY_DIR}/_webview2")
set(_wv2_pkg    "${_wv2_dir}/webview2-${WEBVIEW2_VERSION}.nupkg")
set(_wv2_root   "${_wv2_dir}/pkg")
set(_wv2_inc    "${_wv2_root}/build/native/include")
set(_wv2_header "${_wv2_inc}/WebView2.h")
set(_wv2_lib    "${_wv2_root}/build/native/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${_wv2_header}" OR NOT EXISTS "${_wv2_lib}")
    file(MAKE_DIRECTORY "${_wv2_dir}")
    if(NOT EXISTS "${_wv2_pkg}")
        set(_wv2_url "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg")
        message(STATUS "Downloading WebView2 SDK ${WEBVIEW2_VERSION} from nuget.org ...")
        file(DOWNLOAD "${_wv2_url}" "${_wv2_pkg}" STATUS _wv2_dl TLS_VERIFY ON)
        list(GET _wv2_dl 0 _wv2_code)
        if(NOT _wv2_code EQUAL 0)
            file(REMOVE "${_wv2_pkg}")
            message(FATAL_ERROR "WebView2 SDK download failed (${_wv2_dl}). Check the network / proxy, then re-configure.")
        endif()
    endif()
    message(STATUS "Extracting WebView2 SDK ...")
    file(ARCHIVE_EXTRACT INPUT "${_wv2_pkg}" DESTINATION "${_wv2_root}")
endif()

if(NOT EXISTS "${_wv2_header}" OR NOT EXISTS "${_wv2_lib}")
    message(FATAL_ERROR "WebView2 SDK missing expected files after extraction (${_wv2_header} / ${_wv2_lib}).")
endif()

add_library(webview2 STATIC IMPORTED GLOBAL)
set_target_properties(webview2 PROPERTIES
    IMPORTED_LOCATION "${_wv2_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_wv2_inc}")
# The static loader is compiled with the release static CRT; system deps it needs beyond the usual set.
target_link_libraries(webview2 INTERFACE version shlwapi)
