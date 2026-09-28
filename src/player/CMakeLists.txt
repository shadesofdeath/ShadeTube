add_library(st_player STATIC
    Player.cpp Player.h
)
target_link_libraries(st_player PUBLIC st_audio st_youtube st_catalog)
