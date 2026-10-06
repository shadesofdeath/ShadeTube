# lyrics/ is standalone; the download lyrics frames come from st_audio (Media Foundation libs for its encoder), the
# live mode's Spotify lyrics from st_spotify.
add_executable(lyrics_test lyrics_test.cpp)
target_link_libraries(lyrics_test PRIVATE st_lyrics st_audio st_spotify st_core mfplat mfreadwrite mfuuid ole32)

# The lyrics translation (lyrics/Translate): offline with a stub translator; "live" asks Google once.
add_executable(translate_test translate_test.cpp)
target_link_libraries(translate_test PRIVATE st_lyrics st_core)
