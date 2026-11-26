# Search and Artist page logic (app/BrowseModel): recent searches, the top result, the discography's groups / order.
# Standalone, compiled straight into the test with the catalog JSON helpers and the matcher's fold (app/Shortcuts).
add_executable(browse_test browse_test.cpp ${CMAKE_SOURCE_DIR}/src/app/BrowseModel.cpp ${CMAKE_SOURCE_DIR}/src/app/CatalogJson.cpp
                           ${CMAKE_SOURCE_DIR}/src/app/Shortcuts.cpp)
target_link_libraries(browse_test PRIVATE st_catalog st_core)
