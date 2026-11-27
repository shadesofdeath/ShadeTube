# Deezer / Apple Music / TIDAL lists (app/ServiceImport, standalone) and their matching to a catalog (app/CatalogMatch,
# standalone): link forms, parsers and paging on recorded answers (fixtures/, record_fixtures.py), the scoring.
# `services_test live <link> [ytm|mb|spotify]` / `live-set` read and match real public lists.
add_executable(services_test services_test.cpp
               ${CMAKE_SOURCE_DIR}/src/app/ServiceImport.cpp
               ${CMAKE_SOURCE_DIR}/src/app/CatalogMatch.cpp
               ${CMAKE_SOURCE_DIR}/src/app/Links.cpp
               ${CMAKE_SOURCE_DIR}/src/app/PlaylistIO.cpp)
target_link_libraries(services_test PRIVATE st_spotify st_ytmusic st_musicbrainz st_catalog st_core)
target_compile_definitions(services_test PRIVATE SERVICES_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
