# Song credits: the model (catalog/Credits), the Spotify and MusicBrainz parsers, and app/Credits (standalone: the
# provider chain and its cache), compiled straight into the test.
add_executable(credits_test credits_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Credits.cpp)
target_link_libraries(credits_test PRIVATE st_spotify st_musicbrainz st_catalog st_core shell32)
target_compile_definitions(credits_test PRIVATE CREDITS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
