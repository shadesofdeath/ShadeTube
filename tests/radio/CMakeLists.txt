# Internet radio (app/InternetRadio, standalone): radio-browser.info JSON parsing from saved fixtures, the codec filter,
# station -> track mapping, the favorites / recently played store and, in `live` mode, real API calls.
add_executable(radio_test radio_test.cpp ${CMAKE_SOURCE_DIR}/src/app/InternetRadio.cpp)
target_link_libraries(radio_test PRIVATE st_catalog st_core)
target_compile_definitions(radio_test PRIVATE ST_RADIO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
