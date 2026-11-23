# Theme studio core (gfx/ThemeSpec + app/ThemeStore): palette JSON parsing / writing, contrast of every bundled preset,
# key-color derivation, import validation, the custom theme folder. Standalone (no device, no window), compiled straight
# into the test; the presets are read from the source tree's assets/.
add_executable(theme_test theme_test.cpp ${CMAKE_SOURCE_DIR}/src/gfx/ThemeSpec.cpp ${CMAKE_SOURCE_DIR}/src/gfx/Types.cpp
                          ${CMAKE_SOURCE_DIR}/src/app/ThemeStore.cpp)
target_include_directories(theme_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(theme_test PRIVATE st_core)
target_compile_definitions(theme_test PRIVATE ST_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
