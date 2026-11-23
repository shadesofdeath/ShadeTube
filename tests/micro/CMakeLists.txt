# Micro-interactions: ui/Micro's logic (motion gate, hover fade, equalizer clock and levels, heart pop, card lift) on the
# frame clock (ui/Anim). Standalone, compiled straight into the test (no window, no device).
add_executable(micro_test micro_test.cpp ${CMAKE_SOURCE_DIR}/src/ui/Micro.cpp ${CMAKE_SOURCE_DIR}/src/ui/Anim.cpp
                          ${CMAKE_SOURCE_DIR}/src/ui/ScrollPhysics.cpp)
target_link_libraries(micro_test PRIVATE st_options)
