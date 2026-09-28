add_executable(downloads_test downloads_test.cpp)
target_link_libraries(downloads_test PRIVATE st_audio st_core mfplat mfreadwrite mfuuid ole32)
