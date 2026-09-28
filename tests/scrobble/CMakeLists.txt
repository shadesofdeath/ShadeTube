add_executable(scrobble_test scrobble_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Scrobbler.cpp ${CMAKE_SOURCE_DIR}/src/app/DiscordRpc.cpp)
target_link_libraries(scrobble_test PRIVATE st_catalog st_core bcrypt crypt32)
