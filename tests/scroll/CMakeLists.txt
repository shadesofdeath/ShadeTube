# Smooth scrolling: ui/ScrollPhysics stepped with explicit times, and the frame-clock wrapper (ui/Anim). Standalone,
# compiled straight into the test (no window, no device).
add_executable(scroll_test scroll_test.cpp ${CMAKE_SOURCE_DIR}/src/ui/ScrollPhysics.cpp ${CMAKE_SOURCE_DIR}/src/ui/Anim.cpp)
target_link_libraries(scroll_test PRIVATE st_options)
