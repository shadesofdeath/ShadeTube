# SMTC module test. The ShadeTube exe cannot be linked, so app/Smtc.cpp is compiled in directly.
add_executable(smtc_test smtc_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Smtc.cpp)
target_link_libraries(smtc_test PRIVATE st_catalog st_core windowsapp)
