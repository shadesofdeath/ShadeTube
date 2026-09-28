# Pasted links (app/Links, standalone) and the Spotify id helpers + track metadata parser the link opener uses.
# `links_test live [uri]` looks one track up with the profile's saved sp_dc (read-only).
add_executable(links_test links_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Links.cpp)
target_link_libraries(links_test PRIVATE st_spotify)
