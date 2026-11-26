# Several Spotify accounts (spotify/Accounts + the Session's switching), offline: a per-process temp profile folder and a
# fake connect instead of Spotify.
add_executable(accounts_test accounts_test.cpp)
target_link_libraries(accounts_test PRIVATE st_spotify)
