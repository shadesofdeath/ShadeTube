# Last.fm / ListenBrainz history import (app/ScrobbleImport) on ListenStats + the Spotify import's ZIP reader: standalone,
# compiled straight into the test like stats_test.
add_executable(history_test history_test.cpp ${CMAKE_SOURCE_DIR}/src/app/ScrobbleImport.cpp
                            ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp ${CMAKE_SOURCE_DIR}/src/app/HistoryImport.cpp
                            ${CMAKE_SOURCE_DIR}/src/app/Updater.cpp)
target_link_libraries(history_test PRIVATE st_catalog st_core)
target_compile_definitions(history_test PRIVATE ST_HISTORY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
