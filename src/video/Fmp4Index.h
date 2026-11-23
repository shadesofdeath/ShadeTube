#pragma once
// The segment index of a fragmented MP4 (YouTube's DASH picture streams: ftyp, moov, sidx, then moof + mdat pairs),
// so video mode can seek without Media Foundation scanning every fragment up to the target over the network (its
// MPEG-4 source ignores sidx: a seek to 3:00 read ~40 fragments' headers, each a new ranged request).
//
// A seek instead opens the source on a view of the file: the init boxes (ftyp + moov, from memory) followed by the
// bytes from the segment that holds the target onwards. Segments start with a key frame, so decoding begins at once.
// Pure parsing over bytes (tests/video feeds it made-up boxes).
#include <cstdint>
#include <span>
#include <vector>

namespace st::video {

struct Fmp4Segment {
    int64_t offset = 0;        // byte offset of its first box (moof) in the file
    int64_t size = 0;
    int64_t startUs = 0;       // presentation time it starts at
    int64_t durationUs = 0;
};

// YouTube's streams also carry an edit list (moov/trak/edts/elst: the presentation starts 2 frames, 40 ms, into the
// media, the B-frame delay). Media Foundation's MPEG-4 source presents NO sample of a fragmented track with an edit
// list (the edit's duration is measured against the moov's empty sample tables): its source returned end of stream at
// once on every YouTube video stream. The init boxes are therefore served with the edts box renamed to "free" (same
// size, nothing moves) and the edit's media time is taken off the frames' times by the decoder instead.
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

} // namespace st::video
