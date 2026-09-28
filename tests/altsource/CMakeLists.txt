add_executable(altsource_test altsource_test.cpp)
target_link_libraries(altsource_test PRIVATE st_youtube ws2_32)
target_compile_definitions(altsource_test PRIVATE ST_ALTSOURCE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
