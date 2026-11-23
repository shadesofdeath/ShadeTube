# Now Playing's backdrop colors (app/CoverGradient): palette extraction, the text contrast band, the drifting field.
# Standalone, compiled straight into the test with gfx/Types (Color, HSL); no window, no device.
add_executable(nowplaying_test nowplaying_test.cpp ${CMAKE_SOURCE_DIR}/src/app/CoverGradient.cpp ${CMAKE_SOURCE_DIR}/src/gfx/Types.cpp)
target_link_libraries(nowplaying_test PRIVATE st_options)
