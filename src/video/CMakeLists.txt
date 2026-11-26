# st_video: video mode's picture (the music video of the song, silent, slaved to the audio clock). Reuses st_audio's
# ranged download (ProgressiveBuffer) and IMFByteStream (MfByteStream); decodes with Media Foundation on gfx::Device's
# D3D11 device. The stream choice itself lives in st_youtube (VideoStreams).
add_library(st_video STATIC
    SyncScheduler.cpp SyncScheduler.h   # A/V scheduler: frame to show for the audio clock, drops, resyncs (pure)
    Fmp4Index.h                         # st_audio's fragmented MP4 index under the video namespace
    VideoDecoder.cpp VideoDecoder.h     # download -> MF Source Reader (DXVA) -> NV12 slots, own thread
    VideoPlayer.cpp VideoPlayer.h       # UI side: scheduler + GPU NV12 -> BGRA -> ID2D1Bitmap1
    Soundtrack.cpp Soundtrack.h         # an audio stream decoded whole to 12 kHz mono (the official video's alignment)
    AudioAlign.cpp AudioAlign.h         # song <-> music video soundtrack alignment, the time map (pure)
)
target_link_libraries(st_video
    PUBLIC st_core st_gfx
    PRIVATE st_audio mfplat mfreadwrite mfuuid propsys d3d11 dxgi)
