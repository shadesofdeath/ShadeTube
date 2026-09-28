add_executable(sponsorblock_test sponsorblock_test.cpp ${CMAKE_SOURCE_DIR}/src/app/SponsorBlock.cpp)
target_link_libraries(sponsorblock_test PRIVATE st_core bcrypt)
