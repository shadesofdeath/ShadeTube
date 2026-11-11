# Tag editor core (audio/TagWriter): MP3 / FLAC / M4A round trips on generated files, preservation of everything not
# edited, the audio payload, covers, Unicode, padding, moov before / after mdat, corrupt / read-only / in-use files, and
# the Windows property system's view of the result (compiles app/LocalLibrary.cpp directly, like localfiles_test).
add_executable(tags_test tags_test.cpp ${CMAKE_SOURCE_DIR}/src/app/LocalLibrary.cpp)
target_link_libraries(tags_test PRIVATE st_catalog st_audio st_core mfplat mfreadwrite mfuuid ole32 windowscodecs)
