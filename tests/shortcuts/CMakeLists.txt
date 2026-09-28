# Keyboard shortcuts + autostart: both standalone (core only), compiled straight into the test.
add_executable(shortcuts_test shortcuts_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Shortcuts.cpp ${CMAKE_SOURCE_DIR}/src/app/Autostart.cpp)
target_link_libraries(shortcuts_test PRIVATE st_core)
