add_executable(mb_test main.cpp)
target_link_libraries(mb_test PRIVATE st_musicbrainz)
target_compile_definitions(mb_test PRIVATE MB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
