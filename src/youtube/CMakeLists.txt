add_library(st_youtube STATIC
    AltSource.cpp AltSource.h
    MatchService.cpp MatchService.h
    SoundCloud.cpp SoundCloud.h
    VideoStreams.cpp VideoStreams.h   # video mode: picture stream choice, still-picture check (pure)
)
target_link_libraries(st_youtube PUBLIC st_core st_catalog)
