#pragma once
// Fragmented MP4 (YouTube's DASH streams: ftyp, moov, sidx, then moof + mdat pairs) helpers. Pure parsing over bytes
// (tests/video and tests/codecs feed them made-up boxes and generated files).
//
// The segment index (parseFmp4Index), so video mode can seek without Media Foundation scanning every fragment up to
// the target over the network (its MPEG-4 source ignores sidx: a seek to 3:00 read ~40 fragments' headers, each a new
// ranged request). A seek instead opens the source on a view of the file: the init boxes (ftyp + moov, from memory)
// followed by the bytes from the segment that holds the target onwards. Segments start with a key frame, so decoding
// begins at once.
//
// Edit lists: YouTube's streams can carry one (moov/trak/edts/elst: the picture streams start 2 frames, 40 ms, into
// the media, the B-frame delay; some Art Tracks' AAC streams skip the encoder's priming, 36-74 ms). Media Foundation's
// MPEG-4 source presents NO sample of a fragmented track with an edit list (the edit's duration is measured against
// the moov's empty sample tables): its source returns end of stream at once. The init boxes are therefore served with
// the edts box renamed to "free" (same size, nothing moves) and the edit's media time is taken off the decoded times
// by the reader instead (video/VideoDecoder, audio/Decoder).
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace st::audio {

struct Fmp4Segment {
    int64_t offset = 0;        // byte offset of its first box (moof) in the file
    int64_t size = 0;
    int64_t startUs = 0;       // presentation time it starts at
    int64_t durationUs = 0;
};

struct Fmp4Index {
    std::vector<uint8_t> init;           // ftyp + moov (the boxes before sidx / the first moof, sidx left out, edts freed)
    int64_t editUs = 0;                  // the edit list's media time: subtract from the decoded times
    std::vector<Fmp4Segment> segments;
    bool valid() const { return !init.empty() && segments.size() >= 2; }
    // The segment holding `us` (the last one starting at or before it); 0 before the first.
    size_t segmentAt(int64_t us) const;
};

// Parses the start of the file (`head`: at least up to the end of sidx; 64 KB is plenty for YouTube's files).
// `fileLength`: the whole file's size. Returns an invalid index (no segments) when it is not a fragmented MP4 with a
// sidx in `head`.
Fmp4Index parseFmp4Index(std::span<const uint8_t> head, int64_t fileLength);

// A fragmented MP4 (moov with mvex) whose track has an edit list with a positive media time: its init boxes (the file
// up to the end of moov) with the edts freed, where the rest of the file starts, and the edit's media time. nullopt
// for anything else (no edit list, an ordinary MP4, the moov not complete in `head`): nothing to fix.
struct Fmp4EditFix {
    std::vector<uint8_t> init;
    int64_t bodyOffset = 0;
    int64_t editUs = 0;
};
std::optional<Fmp4EditFix> freeFragmentedEditList(std::span<const uint8_t> head);

} // namespace st::audio
