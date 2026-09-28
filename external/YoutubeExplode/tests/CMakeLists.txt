add_executable(yte-tests
    TestMain.cpp
    UtilsTests.cpp
    CipherTests.cpp
    PipelineTests.cpp
    MusicTests.cpp)
target_link_libraries(yte-tests PRIVATE YoutubeExplode::YoutubeExplode)
# Tests exercise internal components too.
target_include_directories(yte-tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party ${PROJECT_SOURCE_DIR}/example)
if(MSVC)
    target_compile_options(yte-tests PRIVATE /utf-8 /bigobj)
endif()
add_test(NAME yte-tests COMMAND yte-tests)
