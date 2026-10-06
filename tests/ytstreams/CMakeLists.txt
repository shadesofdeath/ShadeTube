add_executable(ytstreams_test ytstreams_test.cpp)
target_link_libraries(ytstreams_test PRIVATE st_youtube st_audio mfplat mfreadwrite mfuuid ole32)
