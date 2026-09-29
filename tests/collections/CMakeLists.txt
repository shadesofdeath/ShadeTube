# Playlist files (app/PlaylistIO, standalone): writers and readers of M3U8 / CSV / XSPF / ShadeTube JSON, the CSV
# dialects of common Spotify exporters, encodings, relative paths and a large-file timing.
add_executable(collections_test collections_test.cpp ${CMAKE_SOURCE_DIR}/src/app/PlaylistIO.cpp
                                ${CMAKE_SOURCE_DIR}/src/app/Links.cpp)
target_link_libraries(collections_test PRIVATE st_catalog st_core)
