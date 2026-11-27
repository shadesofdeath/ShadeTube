add_library(st_lyrics STATIC
    Lyrics.cpp Lyrics.h
    LocalLyrics.cpp
    Translate.cpp Translate.h
    LyricsSync.cpp LyricsSync.h
    LrclibPublish.cpp LrclibPublish.h
    LyricSearch.cpp LyricSearch.h
    Romanize.cpp Romanize.h
)
# shell32 / propsys / ole32: System.Music.Lyrics of non-MP3 local files (Windows property system).
# elscore: offline language detection of lyrics (Extended Linguistic Services), for the translation.
# windowsapp: C++/WinRT, Windows.Globalization.JapanesePhoneticAnalyzer (kanji readings for the romanization).
target_link_libraries(st_lyrics PUBLIC st_core PRIVATE shell32 propsys ole32 elscore windowsapp)
