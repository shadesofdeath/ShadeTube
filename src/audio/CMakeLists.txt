# st_audio: playback engine (AudioEngine) + queue logic (Player, added later).
# Internal pieces (ProgressiveBuffer, MfByteStream, Decoder, Track, WasapiOutput, Spectrum) are
# implementation details of AudioEngine; only AudioEngine.h (and later Player.h) are public.
add_library(st_audio STATIC
    AudioEngine.cpp AudioEngine.h
    ProgressiveBuffer.cpp ProgressiveBuffer.h   # range-request download into one contiguous buffer
    MfByteStream.cpp MfByteStream.h             # IMFByteStream over the buffer
    Fmp4Index.cpp Fmp4Index.h                   # fragmented MP4: sidx segment index, edit lists freed (pure)
    Decoder.cpp Decoder.h                       # built-in or MF Source Reader -> float PCM
    XiphDecoder.cpp XiphDecoder.h               # built-in: Ogg Opus / Vorbis, WebM / Matroska Opus / Vorbis
    XiphCodecs.cpp XiphCodecs.h                 # libopus / libvorbis packet decoders, downmix, Vorbis comments
    Matroska.cpp Matroska.h                     # minimal WebM / Matroska audio demuxer
    Resampler.cpp Resampler.h                   # windowed-sinc rate conversion (built-in decoders)
    XiphTags.cpp XiphTags.h                     # Ogg Opus / Vorbis file info for the local library (tags, cover)
    Track.cpp Track.h                           # buffer + decode thread + PCM ring
    WasapiOutput.cpp WasapiOutput.h             # WASAPI event-driven render client (shared / exclusive)
    Spectrum.cpp Spectrum.h                     # Hann + radix-2 FFT -> log bands; the waveform tap (seqlock)
    Fft.cpp Fft.h                               # the 2048-point FFT + Hann window (spectrum, visualizer)
    VisualAnalysis.cpp VisualAnalysis.h         # the visualizer's bands, peaks, energies, beats, waveform (pure)
    Equalizer.cpp Equalizer.h                   # 10-band peaking biquads + presets (also drawn by Ayarlar > SES)
    ReplayGain.cpp ReplayGain.h                 # ReplayGain track gain from ID3 / FLAC / MP4 tags (local files)
    Limiter.cpp Limiter.h                       # look-ahead output limiter (-0.3 dBFS ceiling)
    TimeStretch.cpp TimeStretch.h               # playback speed without a pitch change (WSOLA)
    Downloader.cpp Downloader.h                 # offline download + MF PCM->MP3 transcode + ID3
    TagWriter.cpp TagWriter.h                   # tag editor core: ID3v2 / FLAC / MP4 read + safe rewrite
    LiveStream.cpp LiveStream.h                 # live radio: playlists, ICY / HLS, rolling buffer, reconnects
    LiveParsers.cpp LiveParsers.h               # MPEG / ADTS / Ogg framing, ICY, playlists, HLS, TS, fMP4
    LiveDecoder.cpp LiveDecoder.h               # decoder MFT fed one compressed frame at a time
    HttpStream.cpp HttpStream.h                 # sequential WinHTTP reader (+ socket for "ICY 200 OK")
)
target_link_libraries(st_audio
    PUBLIC st_core
    PRIVATE mfplat mfreadwrite mfuuid ole32 avrt winhttp advapi32 ws2_32 bcrypt
            xiph_opusfile xiph_vorbis xiph_opus xiph_ogg)
