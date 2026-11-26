# Song cache (player/AudioCache): store, checks, eviction, the playback buffer's completion hook against an in-process
# HTTP server, the engine and a real Player with the network simulated away (SHADETUBE_OFFLINE).
add_executable(audiocache_test audiocache_test.cpp)
target_link_libraries(audiocache_test PRIVATE st_player st_audio st_core ws2_32)
