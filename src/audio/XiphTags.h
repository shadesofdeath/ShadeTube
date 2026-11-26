#pragma once
// What the local library needs from an Ogg Opus / Ogg Vorbis file (st_audio; app/LocalLibrary and tests use it): the
// Vorbis comments, the exact duration (opusfile / vorbisfile: the last granule position, pre-skip taken off, chained
// files summed) and the embedded cover (METADATA_BLOCK_PICTURE, the front cover preferred). Stock Windows has no
// property handler for these files, so this replaces the property store for them. Blocking file I/O: a worker thread.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace st::audio::xiph {

struct FileInfo {
    std::string codec;               // "Opus" / "Vorbis"
    int64_t durationMs = 0;
    uint32_t sampleRate = 0;         // Opus: the encoder's input rate when given (it decodes at 48 kHz)
    uint32_t channels = 0;
    std::vector<std::pair<std::string, std::string>> tags;   // (KEY in upper case, value), in file order
    std::string picture;             // the embedded cover's image bytes (only when asked for)

    std::string tag(std::string_view key) const;                 // the first value of `key`, "" if none
    std::vector<std::string> tagValues(std::string_view key) const;
};

// nullopt: not an Ogg Opus / Vorbis file, or unreadable.
std::optional<FileInfo> readOggFile(const std::filesystem::path& path, bool withPicture);

} // namespace st::audio::xiph
