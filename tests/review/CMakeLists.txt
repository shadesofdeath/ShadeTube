# Year in review (app/YearReview) on synthetic histories; ListenStats is compiled in (both standalone).
add_executable(review_test review_test.cpp ${CMAKE_SOURCE_DIR}/src/app/YearReview.cpp ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp)
target_link_libraries(review_test PRIVATE st_catalog st_core)
