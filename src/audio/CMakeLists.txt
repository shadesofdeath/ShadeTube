# st_audio: playback engine (AudioEngine) + queue logic (Player, added later).
# Internal pieces (ProgressiveBuffer, MfByteStream, Decoder, Track, WasapiOutput, Spectrum) are
# implementation details of AudioEngine; only AudioEngine.h (and later Player.h) are public.
add_library(st_audio STATIC
    AudioEngine.cpp AudioEngine.h
    ProgressiveBuffer.cpp ProgressiveBuffer.h   # range-request download into one contiguous buffer
    MfByteStream.cpp MfByteStream.h             # IMFByteStream over the buffer
    Decoder.cpp Decoder.h                       # MF Source Reader -> float PCM
    Track.cpp Track.h                           # buffer + decode thread + PCM ring
    WasapiOutput.cpp WasapiOutput.h             # WASAPI shared, event-driven render client
    Spectrum.cpp Spectrum.h                     # Hann + radix-2 FFT -> log bands
    Equalizer.cpp Equalizer.h                   # 10-band peaking biquads + presets (also drawn by Ayarlar > SES)
    ReplayGain.cpp ReplayGain.h                 # ReplayGain track gain from ID3 / FLAC / MP4 tags (local files)
    Limiter.cpp Limiter.h                       # look-ahead output limiter (-0.3 dBFS ceiling)
    TimeStretch.cpp TimeStretch.h               # playback speed without a pitch change (WSOLA)
    Downloader.cpp Downloader.h                 # offline download + MF PCM->MP3 transcode + ID3
    LiveStream.cpp LiveStream.h                 # live radio: playlists, ICY / HLS, rolling buffer, reconnects
    LiveParsers.cpp LiveParsers.h               # MPEG / ADTS / Ogg framing, ICY, playlists, HLS, TS, fMP4
    LiveDecoder.cpp LiveDecoder.h               # decoder MFT fed one compressed frame at a time
    HttpStream.cpp HttpStream.h                 # sequential WinHTTP reader (+ socket for "ICY 200 OK")
)
target_link_libraries(st_audio
    PUBLIC st_core
    PRIVATE mfplat mfreadwrite mfuuid ole32 avrt winhttp advapi32 ws2_32 bcrypt)
