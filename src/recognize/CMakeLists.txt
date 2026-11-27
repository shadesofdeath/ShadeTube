# st_recognize: song recognition ("Şarkıyı tanı"): what plays on this PC (WASAPI loopback) or in the room (the
# microphone) -> a Shazam-format signature -> the recognition service's answer. Only the signature leaves the machine.
add_library(st_recognize STATIC
    Signature.cpp Signature.h     # spectral peaks of 16 kHz mono audio, Shazam's binary signature format (pure)
    Shazam.cpp Shazam.h           # the recognition request and its JSON answer
    Capture.cpp Capture.h         # WASAPI loopback (other apps' sound) / microphone capture -> 16 kHz mono
    Session.cpp Session.h         # one listening run: capture, growing signatures, requests (its own thread)
)
target_link_libraries(st_recognize PUBLIC st_core PRIVATE st_audio ole32 avrt mmdevapi)
