add_library(st_youtube STATIC
    AltSource.cpp AltSource.h
    MatchService.cpp MatchService.h
    SoundCloud.cpp SoundCloud.h
    VideoStreams.cpp VideoStreams.h   # video mode: picture stream choice, still-picture check (pure)
    OfficialVideo.cpp OfficialVideo.h # video mode: the official music video of a song (YouTube Music search, ranking)
)
target_link_libraries(st_youtube PUBLIC st_core st_catalog)
