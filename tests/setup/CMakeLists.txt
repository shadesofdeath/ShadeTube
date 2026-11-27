# The setup (src/setup): payload pack / read / unpack and its damage checks, the command line, and install / update /
# uninstall into a SHADETUBE_SETUP_SANDBOX (files, test registry key, shortcuts, Run value), plus ShadeTubeSetup.exe /S
# itself in a sandbox. ST_SETUP_EXE / ST_APP_EXE: the setup and the app of this build tree (the app honours the
# sandbox's variables; never a packaged release).
add_executable(setup_test setup_test.cpp)
target_link_libraries(setup_test PRIVATE st_setup_core)
target_compile_definitions(setup_test PRIVATE ST_SETUP_EXE="$<TARGET_FILE:ShadeTubeSetup>" ST_APP_EXE="$<TARGET_FILE:ShadeTube>")
add_dependencies(setup_test ShadeTubeSetup ShadeTube)
