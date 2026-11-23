# Collapsing page headers: app/HeaderCollapse (geometry per scroll offset, the tint's contrast guard). Standalone, compiled
# straight into the test with gfx/Types (colors) — no window, no device.
add_executable(header_collapse_test header_collapse_test.cpp ${CMAKE_SOURCE_DIR}/src/app/HeaderCollapse.cpp
                                    ${CMAKE_SOURCE_DIR}/src/gfx/Types.cpp)
target_link_libraries(header_collapse_test PRIVATE st_options)
