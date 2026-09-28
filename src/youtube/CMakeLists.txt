add_library(st_youtube STATIC
    AltSource.cpp AltSource.h
    MatchService.cpp MatchService.h
)
target_link_libraries(st_youtube PUBLIC st_core st_catalog)
