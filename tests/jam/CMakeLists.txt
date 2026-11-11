# "Birlikte dinle" (listen together): the standalone core (MQTT codec + client, crypto, invite links, protocol / state,
# reconciliation, clock). `jam_test live [wss://broker...]` talks to the real public brokers.
add_executable(jam_test jam_test.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamCrypto.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamInvite.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamMqtt.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamProtocol.cpp
    ${CMAKE_SOURCE_DIR}/src/app/JamSync.cpp)
target_link_libraries(jam_test PRIVATE st_core bcrypt winhttp)
