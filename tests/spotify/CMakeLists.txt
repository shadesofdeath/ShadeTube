add_executable(spotify_test spotify_test.cpp ${CMAKE_SOURCE_DIR}/src/app/PlaylistEditing.cpp
                            ${CMAKE_SOURCE_DIR}/src/app/CatalogJson.cpp)
target_link_libraries(spotify_test PRIVATE st_spotify windowscodecs ole32)
target_compile_definitions(spotify_test PRIVATE SPOTIFY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
