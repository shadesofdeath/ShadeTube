#pragma once
// ReplayGain track gain of a local audio file, for loudness normalisation (internal to st_audio; tests use it too).
//
// Only the tag areas are read, seeking over everything else (covers, audio): ID3v2 at the start of the file
// (TXXX "REPLAYGAIN_TRACK_GAIN"; MP3, and AAC / FLAC files with a leading ID3 tag), FLAC Vorbis comments
// ("REPLAYGAIN_TRACK_GAIN=") and MP4 / M4A iTunes freeform atoms (----:com.apple.iTunes:replaygain_track_gain).
// ReplayGain 2 gains take a track to -18 LUFS. Blocking file I/O: call from a worker / decode thread.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace st::audio::replaygain {

// The file's track gain in dB, nullopt when it has none (or the file can't be read).
std::optional<float> readTrackGainDb(const std::filesystem::path& file);

// ---- exposed for tests ----
// "-6.54 dB", "+1.20 dB", "-6,54dB", " 3 " -> dB; nullopt for anything else or implausible values (|x| > 64).
std::optional<float> parseGain(std::string_view text);
// The same lookups on in-memory data, each starting at the beginning of a file.
std::optional<float> fromId3(const uint8_t* data, size_t size);
std::optional<float> fromFlac(const uint8_t* data, size_t size);
std::optional<float> fromMp4(const uint8_t* data, size_t size);

} // namespace st::audio::replaygain
