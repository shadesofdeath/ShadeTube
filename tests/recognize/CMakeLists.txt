# Song recognition (src/recognize + app/Recognition): the signature of a synthetic signal with known peaks, the binary
# format, recorded service answers, the session on test audio with a fake service, the catalog matching and the
# history store (offline); live modes against the service. app/Recognition is standalone and compiled in directly (with
# CatalogJson).
add_executable(recognize_test recognize_test.cpp
    ${CMAKE_SOURCE_DIR}/src/app/Recognition.cpp ${CMAKE_SOURCE_DIR}/src/app/CatalogJson.cpp)
target_link_libraries(recognize_test PRIVATE st_recognize st_youtube st_musicbrainz st_audio st_catalog st_core
    mfplat mfreadwrite mfuuid ole32 shell32)
target_compile_definitions(recognize_test PRIVATE ST_RECOGNIZE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
