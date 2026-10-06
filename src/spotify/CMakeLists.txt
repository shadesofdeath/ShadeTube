add_library(st_spotify STATIC
    Totp.cpp Totp.h
    Auth.cpp Auth.h
    HashRegistry.cpp HashRegistry.h
    PlaylistTree.cpp PlaylistTree.h
    SpotifyApi.cpp SpotifyApi.h
    Social.cpp
    Session.cpp Session.h
)
# bcrypt: HMAC-SHA1 for TOTP.  crypt32: DPAPI for the sp_dc cookie at rest.
target_link_libraries(st_spotify PUBLIC st_catalog st_core PRIVATE bcrypt crypt32)
