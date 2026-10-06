# Playlist files (app/PlaylistIO, standalone): writers and readers of M3U8 / CSV / XSPF / ShadeTube JSON, the CSV
# dialects of common Spotify exporters, encodings, relative paths and a large-file timing.
add_executable(collections_test collections_test.cpp ${CMAKE_SOURCE_DIR}/src/app/PlaylistIO.cpp
                                ${CMAKE_SOURCE_DIR}/src/app/Links.cpp)
target_link_libraries(collections_test PRIVATE st_catalog st_core)
# Playlist editing (app/PlaylistEditing, standalone): reordering a multi-selection and the position Spotify gets, undo of
# a removal, cover pictures (centered square JPEG within Spotify's 256 KB), local playlists' library.json entry + covers.
add_executable(playlist_edit_test playlist_edit_test.cpp ${CMAKE_SOURCE_DIR}/src/app/PlaylistEditing.cpp
                                  ${CMAKE_SOURCE_DIR}/src/app/CatalogJson.cpp)
target_link_libraries(playlist_edit_test PRIVATE st_catalog st_core windowscodecs ole32)
