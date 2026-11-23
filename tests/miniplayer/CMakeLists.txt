# Mini player geometry (app/MiniLayout): standalone (windows.h only), compiled straight into the test. st_core only
# for the common compile settings (include path, NOMINMAX, UNICODE).
add_executable(miniplayer_test miniplayer_test.cpp ${CMAKE_SOURCE_DIR}/src/app/MiniLayout.cpp)
target_link_libraries(miniplayer_test PRIVATE st_core)
