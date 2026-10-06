# The "Yeni çıkanlar" model (app/NewReleases) is standalone: compiled straight into the test, no app target needed.
add_executable(newreleases_test newreleases_test.cpp ${CMAKE_SOURCE_DIR}/src/app/NewReleases.cpp)
target_link_libraries(newreleases_test PRIVATE st_catalog st_core)
