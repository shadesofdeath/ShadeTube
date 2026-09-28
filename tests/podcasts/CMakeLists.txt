# Podcasts (app/Podcasts, standalone): the XML reader, RSS / directory parsing, dates and durations, the episode model,
# download naming, the podcasts.json store and, in `live` mode, Apple's directory, a real feed and its audio.
add_executable(podcasts_test podcasts_test.cpp ${CMAKE_SOURCE_DIR}/src/app/Podcasts.cpp)
target_link_libraries(podcasts_test PRIVATE st_catalog st_core)
