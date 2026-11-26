add_library(st_ytmusic STATIC
    Parse.cpp Parse.h
    YtMusic.cpp YtMusic.h
)
target_link_libraries(st_ytmusic PUBLIC st_catalog st_core)
