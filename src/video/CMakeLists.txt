# st_video: video mode's picture (the music video of the song, silent, slaved to the audio clock). Reuses st_audio's
# ranged download (ProgressiveBuffer) and IMFByteStream (MfByteStream); decodes with Media Foundation on gfx::Device's
# D3D11 device. The stream choice itself lives in st_youtube (VideoStreams).
add_library(st_video STATIC
    SyncScheduler.cpp SyncScheduler.h   # A/V scheduler: frame to show for the audio clock, drops, resyncs (pure)
    Fmp4Index.cpp Fmp4Index.h           # sidx segment index of a fragmented MP4: seeks without scanning (pure)
    VideoDecoder.cpp VideoDecoder.h     # download -> MF Source Reader (DXVA) -> NV12 slots, own thread
    VideoPlayer.cpp VideoPlayer.h       # UI side: scheduler + GPU NV12 -> BGRA -> ID2D1Bitmap1
)
target_link_libraries(st_video
    PUBLIC st_core st_gfx
    PRIVATE st_audio mfplat mfreadwrite mfuuid propsys d3d11 dxgi)
