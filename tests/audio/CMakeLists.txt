add_executable(audio_test audio_test.cpp)
target_link_libraries(audio_test PRIVATE st_audio YoutubeExplode::YoutubeExplode)

add_executable(mp3_probe mp3_probe.cpp)
target_link_libraries(mp3_probe PRIVATE st_options mfplat mfreadwrite mfuuid ole32)
