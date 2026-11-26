add_executable(video_test video_test.cpp ${CMAKE_SOURCE_DIR}/src/app/OfficialVideoCache.cpp)
target_link_libraries(video_test PRIVATE st_video st_youtube st_audio st_gfx mfplat mfreadwrite mfuuid ole32)
target_include_directories(video_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(video_test PRIVATE ST_VIDEO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
