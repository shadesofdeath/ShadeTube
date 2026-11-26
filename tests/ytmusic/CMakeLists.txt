add_executable(ytmusic_test ytmusic_test.cpp)
target_link_libraries(ytmusic_test PRIVATE st_ytmusic st_youtube)
target_compile_definitions(ytmusic_test PRIVATE YTM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
