# Yerel dosyalar: scanner / tags / covers / index / incremental rescan (compiles app/LocalLibrary.cpp directly)
# + Media Foundation decode of each local format and a short real playback through the AudioEngine.
add_executable(localfiles_test localfiles_test.cpp ${CMAKE_SOURCE_DIR}/src/app/LocalLibrary.cpp)
target_link_libraries(localfiles_test PRIVATE st_catalog st_audio st_core mfplat mfreadwrite mfuuid ole32)
