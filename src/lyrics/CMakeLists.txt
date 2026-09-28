add_library(st_lyrics STATIC
    Lyrics.cpp Lyrics.h
)
target_link_libraries(st_lyrics PUBLIC st_core)
