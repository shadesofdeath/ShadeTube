# Song-change notification: the rules and the toast XML (app/SongNotification, standalone), the XML checked by WinRT's
# own parser.
add_executable(songnotify_test songnotify_test.cpp ${CMAKE_SOURCE_DIR}/src/app/SongNotification.cpp)
target_link_libraries(songnotify_test PRIVATE st_core windowsapp)
