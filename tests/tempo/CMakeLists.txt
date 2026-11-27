# Tempo estimator (audio/Tempo): synthetic click tracks and drum loops at known tempos (swing, half / double-time
# traps, noise), a generated WAV through the file analysis (decoder + seek + downmix), timing; `file <path...>` for real
# files.
add_executable(tempo_test tempo_test.cpp)
target_link_libraries(tempo_test PRIVATE st_audio st_core mfplat mfreadwrite mfuuid ole32)
