# Live (endless) internet-radio streams: parsers, the audio engine and the Player against mock_icecast.py (Python), and
# in `live` mode a few real public stations. See liveaudio_test.cpp.
add_executable(liveaudio_test liveaudio_test.cpp)
target_link_libraries(liveaudio_test PRIVATE st_player st_audio st_catalog st_core mfplat mfreadwrite mfuuid ole32 winhttp)
target_compile_definitions(liveaudio_test PRIVATE ST_LIVEAUDIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
