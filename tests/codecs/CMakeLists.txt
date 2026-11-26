# Built-in Opus / Vorbis decoding: resampler, Matroska demuxer, the decoder path on ffmpeg-made fixtures (fixtures/,
# regenerate with fixtures/make_fixtures.py), Ogg tags, live Ogg parsing + decoding, a short engine run (offline).
add_executable(codecs_test codecs_test.cpp)
target_link_libraries(codecs_test PRIVATE st_audio st_core mfplat mfreadwrite mfuuid ole32)
target_compile_definitions(codecs_test PRIVATE ST_CODECS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
