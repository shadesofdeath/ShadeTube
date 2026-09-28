# Kara liste store + matching rules (compiles app/Blacklist.cpp directly), Player::localMimeType and a Media Foundation
# decode of local files with the mapped type.
add_executable(playback_test playback_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Blacklist.cpp)
target_link_libraries(playback_test PRIVATE st_player st_catalog st_core mfplat mfreadwrite mfuuid ole32)
