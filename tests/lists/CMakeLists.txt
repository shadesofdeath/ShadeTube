# List design geometry (app/TrackColumns: track table columns, selection runs, queue sections; gfx::skeletonBand).
# Standalone, compiled straight into the test (no window, no device).
add_executable(lists_test lists_test.cpp ${CMAKE_SOURCE_DIR}/src/app/TrackColumns.cpp)
target_link_libraries(lists_test PRIVATE st_options)
