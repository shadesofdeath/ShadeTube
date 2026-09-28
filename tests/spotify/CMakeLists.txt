add_executable(spotify_test spotify_test.cpp)
target_link_libraries(spotify_test PRIVATE st_spotify)
target_compile_definitions(spotify_test PRIVATE SPOTIFY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
