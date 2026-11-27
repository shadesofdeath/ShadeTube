# The full-screen visualizer's analysis (audio/VisualAnalysis): band mapping, smoothing and peaks, energies, the beat
# detector on synthetic music at several display rates, the oscilloscope's trigger; `wav` writes that music to a file.
add_executable(visualizer_test visualizer_test.cpp)
target_link_libraries(visualizer_test PRIVATE st_audio)
