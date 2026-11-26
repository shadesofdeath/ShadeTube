# Blend (app/Blend): taste profiles, the shared playlist and the match on synthetic histories; `blend_test live` exchanges
# two profiles through a real public broker with the members never online at the same time (the Jam core's transport).
add_executable(blend_test blend_test.cpp
    ${CMAKE_SOURCE_DIR}/src/app/Blend.cpp
    ${CMAKE_SOURCE_DIR}/src/app/ListenStats.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamCrypto.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamInvite.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamMqtt.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamProtocol.cpp)
target_link_libraries(blend_test PRIVATE st_catalog st_core bcrypt crypt32 winhttp)
