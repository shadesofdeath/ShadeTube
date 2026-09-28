# ListenStats (app/ListenStats) is standalone: compiled straight into the test, no app target needed.
add_executable(stats_test stats_test.cpp ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp)
target_link_libraries(stats_test PRIVATE st_catalog st_core)
