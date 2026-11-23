# Page transitions: ui/PageMotion (the pages' timeline and the shared cover's flight) stepped with explicit times.
# Standalone, compiled straight into the test (no window, no device).
add_executable(pagetx_test pagetx_test.cpp ${CMAKE_SOURCE_DIR}/src/ui/PageMotion.cpp ${CMAKE_SOURCE_DIR}/src/ui/Anim.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ScrollPhysics.cpp)
target_link_libraries(pagetx_test PRIVATE st_options)
