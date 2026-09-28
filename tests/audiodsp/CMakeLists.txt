# Equalizer math, ReplayGain tag parsing and output-device enumeration (offline).
add_executable(audiodsp_test audiodsp_test.cpp)
target_link_libraries(audiodsp_test PRIVATE st_audio)
