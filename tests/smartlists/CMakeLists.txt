# Smart lists (app/SmartLists) on synthetic histories; ListenStats is compiled in (both standalone).
add_executable(smartlists_test smartlists_test.cpp ${CMAKE_SOURCE_DIR}/src/app/SmartLists.cpp ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp)
target_link_libraries(smartlists_test PRIVATE st_catalog st_core)
