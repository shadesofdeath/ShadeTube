#pragma once
// Reading and writing the tags of the user's own audio files ("Etiketleri düzenle" in Yerel dosyalar). Standalone
// (core only, no UI): tests/tags compiles against st_audio.
//
// Formats, all parsed and rebuilt by our own code (the Windows property system is not used for writing):
//   * MP3: the ID3v2 tag in front of the audio. v2.3 stays v2.3, v2.4 stays v2.4, v2.2 becomes v2.3 (its frames
//     converted). Every frame that isn't edited (USLT, SYLT, TXXX ReplayGain, POPM, PRIV, COMM, ...) is copied byte for
//     byte; a tag-wide unsynchronisation is undone and not written again; the extended header is dropped. A file
//     without an ID3v2 tag gets a v2.3 one. An ID3v1 tag at the end is updated too (Latin-1, best fit).
//   * FLAC: the VORBIS_COMMENT block (vendor and other comments kept) and PICTURE blocks; every other block (STREAMINFO,
//     SEEKTABLE, APPLICATION, CUESHEET) is copied. The metadata keeps its size when the padding can absorb the change
//     (the audio frames stay where they were), otherwise it is rewritten with 4 KB of new padding.
//   * MP4 / M4A (AAC, ALAC): moov/udta/meta/ilst (created when missing); other ilst items (freeform "----" ReplayGain,
//     lyrics, ...) are kept. When the moov box changes size, a free box right behind it absorbs the change, or the
//     chunk offsets (stco / co64; tfhd base offsets and tfra entries of fragmented files) are moved along.
// Other formats (WAV, WMA, ADTS AAC, Ogg / Opus) are not writable.
//
// write() is safe: the new file is written next to the original (a hidden temporary in the same folder), read back
// and checked - the edited fields, every preserved frame / block / item, and a SHA-256 of the audio payload, which
// must be identical - and only then swapped in with ReplaceFileW (with a backup name, so even a failed swap leaves
// the original under its name). Any failure leaves the original untouched. A read-only file is refused; a file another
// program holds open without delete sharing is reported as in use. The playing file is fine: the audio engine opens
// it with FILE_SHARE_DELETE and memory-maps it, and ReplaceFileW keeps that mapping on the old data, so playback
// continues uninterrupted.
//
// Blocking file I/O: call from a worker thread.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace st::audio::tags {

enum class Format { Unknown, Mp3, Flac, Mp4 };
const char* formatName(Format f);   // "MP3" / "FLAC" / "MP4" / ""

// By extension only (no disk access): the formats write() handles (.mp3 .flac .m4a .mp4 .m4b).
bool writableExtension(const std::filesystem::path& file);

struct Picture {
    std::vector<uint8_t> data;   // encoded image (JPEG / PNG)
    std::string mime;            // "image/jpeg" / "image/png"
    int type = 3;                // ID3 / FLAC picture type: 3 = front cover
    bool operator==(const Picture&) const = default;
};

struct Tags {
    std::string title, artist, album, albumArtist, genre;   // UTF-8; several artists are joined with "; "
    int track = 0, trackTotal = 0, disc = 0, discTotal = 0, year = 0;   // 0 = none
    std::optional<Picture> cover;                            // the front cover (else the first picture)
    bool operator==(const Tags&) const = default;
};

struct ReadResult {
    bool ok = false;
    Format format = Format::Unknown;
    Tags tags;
    std::string error;   // when !ok (log text)
};
ReadResult read(const std::filesystem::path& file);

// What to change; unset = leave as it is. Strings are trimmed; an empty string removes the field. Numbers: 0 removes.
struct Edit {
    std::optional<std::string> title, artist, album, albumArtist, genre;
    std::optional<int> track, disc, year;
    enum class Cover { Keep, Set, Remove };
    Cover cover = Cover::Keep;
    Picture picture;   // for Cover::Set (front cover)
    bool empty() const;
};
// The tags `t` become with `e` applied (what write() checks the new file against).
Tags applied(Tags t, const Edit& e);
// "A;B ; C" -> "A; B; C" (how artist lists are written).
std::string normalizeList(const std::string& s);

enum class Status { Ok, Unsupported, NotFound, ReadOnly, InUse, Corrupt, TooLarge, IoError, VerifyFailed, Changed };
const char* toString(Status s);

struct WriteResult {
    Status status = Status::IoError;
    std::string detail;     // log text
    int64_t size = 0;       // the new file (Ok)
    uint64_t mtime = 0;     // its last write time, FILETIME ticks (Ok)
};

struct WriteOptions {
    bool verify = true;     // read the new file back before the swap (tests may turn it off to check the checks)
    // Tests: called with the temporary file before it is verified (to corrupt it on purpose).
    void (*tamper)(const std::filesystem::path& temp) = nullptr;
};
WriteResult write(const std::filesystem::path& file, const Edit& edit, const WriteOptions& options = {});

// ---- exposed for tests -------------------------------------------------------------------------------------------
// SHA-256 of the audio payload (MP3: between the ID3v2 tag and an ID3v1 tag; FLAC: the frames after the metadata;
// MP4: the contents of every mdat box), "" when the file can't be parsed.
std::string payloadHash(const std::filesystem::path& file);
// Width / height of a JPEG or PNG (0, 0 when unknown).
std::pair<int, int> imageSize(const std::vector<uint8_t>& data);
// The ID3v1 genre name of index n ("" when out of range) and the index of a name (-1 = not a standard genre).
std::string id3v1Genre(int n);
int id3v1GenreIndex(const std::string& name);

} // namespace st::audio::tags
