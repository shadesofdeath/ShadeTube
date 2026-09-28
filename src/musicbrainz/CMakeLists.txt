add_library(st_musicbrainz STATIC
    MusicBrainz.cpp MusicBrainz.h
    Net.cpp Net.h
    Parse.cpp Parse.h
)
target_link_libraries(st_musicbrainz PUBLIC st_catalog st_core)
