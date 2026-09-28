add_executable(updater_test updater_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Updater.cpp ${CMAKE_SOURCE_DIR}/src/app/Installer.cpp)
target_link_libraries(updater_test PRIVATE st_core)
# ST_APP_EXE: the app of this build tree. The running-instance checks start it with SHADETUBE_DATA_DIR, which only
# builds that know the variable honour: an older released exe (dist\*.zip) would use the user's real profile.
target_compile_definitions(updater_test PRIVATE ST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                                ST_UPDATER_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
                                                ST_APP_EXE="$<TARGET_FILE:ShadeTube>")
add_dependencies(updater_test ShadeTube)
