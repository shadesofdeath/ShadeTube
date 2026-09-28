# Download sync rules + planning (app/SyncRules) is standalone: compiled straight into the test, no app target needed.
add_executable(sync_test sync_test.cpp ${CMAKE_SOURCE_DIR}/src/app/SyncRules.cpp)
target_link_libraries(sync_test PRIVATE st_catalog st_core)
