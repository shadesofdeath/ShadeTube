add_executable(video_test video_test.cpp)
target_link_libraries(video_test PRIVATE st_video st_youtube st_audio st_gfx mfplat mfreadwrite mfuuid ole32)
