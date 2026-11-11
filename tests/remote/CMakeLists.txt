# Phone remote control (app/RemoteHttp, RemoteAuth, RemoteApi, RemoteState, RemoteServer; all standalone): HTTP parsing,
# pairing / tokens / throttling, routing, SSE framing, state JSON, adapter ranking and a live server on 127.0.0.1
# talked to with WinHTTP. Offline; runs in a per-process temp profile.
add_executable(remote_test remote_test.cpp
    ${CMAKE_SOURCE_DIR}/src/app/RemoteHttp.cpp
    ${CMAKE_SOURCE_DIR}/src/app/RemoteAuth.cpp
    ${CMAKE_SOURCE_DIR}/src/app/RemoteApi.cpp
    ${CMAKE_SOURCE_DIR}/src/app/RemoteState.cpp
    ${CMAKE_SOURCE_DIR}/src/app/RemoteServer.cpp)
target_link_libraries(remote_test PRIVATE st_catalog st_core bcrypt crypt32 ws2_32 iphlpapi winhttp)
