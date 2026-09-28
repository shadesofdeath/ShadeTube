# ListenStats (app/ListenStats) and the history import (app/HistoryImport, with the Updater's ZIP inflate) are
# standalone: compiled straight into the test, no app target needed.
add_executable(stats_test stats_test.cpp ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp ${CMAKE_SOURCE_DIR}/src/app/HistoryImport.cpp
                          ${CMAKE_SOURCE_DIR}/src/app/Updater.cpp)
target_link_libraries(stats_test PRIVATE st_catalog st_core)
target_compile_definitions(stats_test PRIVATE ST_STATS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
