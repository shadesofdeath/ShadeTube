# lyrics/ is standalone; the download lyrics frames come from st_audio (Media Foundation libs for its encoder), the
# live mode's Spotify lyrics from st_spotify.
add_executable(lyrics_test lyrics_test.cpp)
target_link_libraries(lyrics_test PRIVATE st_lyrics st_audio st_spotify st_core mfplat mfreadwrite mfuuid ole32)
