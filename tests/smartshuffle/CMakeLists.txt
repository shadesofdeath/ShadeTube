# Smart shuffle / "Geliştir" planning (compiles app/SmartMix.cpp directly) and the Player's recommendation handling.
add_executable(smartshuffle_test smartshuffle_test.cpp ${CMAKE_SOURCE_DIR}/src/app/SmartMix.cpp)
target_link_libraries(smartshuffle_test PRIVATE st_player st_catalog st_core mfplat mfreadwrite mfuuid ole32)
