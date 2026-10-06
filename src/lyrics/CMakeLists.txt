add_library(st_lyrics STATIC
    Lyrics.cpp Lyrics.h
    LocalLyrics.cpp
    Translate.cpp Translate.h
)
# shell32 / propsys / ole32: System.Music.Lyrics of non-MP3 local files (Windows property system).
# elscore: offline language detection of lyrics (Extended Linguistic Services), for the translation.
target_link_libraries(st_lyrics PUBLIC st_core PRIVATE shell32 propsys ole32 elscore)
