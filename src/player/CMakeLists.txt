add_library(st_player STATIC
    Player.cpp Player.h
    AudioCache.cpp AudioCache.h   # song cache: played streams kept on disk (cache\audio), LRU, verified
)
target_link_libraries(st_player PUBLIC st_audio st_youtube st_catalog)
