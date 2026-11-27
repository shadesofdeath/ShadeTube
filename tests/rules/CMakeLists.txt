# Rule-based playlists (app/RuleLists: definitions + JSON, the pool, evaluation, incremental updates) and the BPM store
# (app/BpmStore); both standalone, compiled straight into the test.
add_executable(rules_test rules_test.cpp ${CMAKE_SOURCE_DIR}/src/app/RuleLists.cpp ${CMAKE_SOURCE_DIR}/src/app/BpmStore.cpp)
target_link_libraries(rules_test PRIVATE st_catalog st_core)
