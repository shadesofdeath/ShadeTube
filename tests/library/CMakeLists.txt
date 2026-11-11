# Library page views (app/LibrarySort): sort orders, folders, the filter, the recently played record. Standalone, compiled
# straight into the test with the matcher it uses (app/Shortcuts) and ListenStats (the history's recency).
add_executable(library_test library_test.cpp ${CMAKE_SOURCE_DIR}/src/app/LibrarySort.cpp ${CMAKE_SOURCE_DIR}/src/app/Shortcuts.cpp
                            ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp)
target_link_libraries(library_test PRIVATE st_catalog st_core)
