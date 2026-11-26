#pragma once
// Minimal Matroska / WebM demuxer for one audio track (internal to st_audio; tests use it too): YouTube's Opus-in-WebM
// streams and local .webm / .weba / .mka files, for the built-in decoders (audio/XiphDecoder).
//
// Reads the EBML header, the Segment's SeekHead, Info (timestamp scale, duration), Tracks (the first audio track: codec,
// CodecPrivate, CodecDelay, SeekPreRoll, rate, channels) and the Cues (read when the first seek needs them, wherever
// they are), then hands out that track's frames from SimpleBlocks and BlockGroups (with DiscardPadding) in file order;
// Xiph, EBML and fixed-size lacing are split into frames. Seeking goes to the last cue (cluster) at or before the
// target, or, without cues, scans the cluster headers. Everything else (video, subtitles, tags, chapters, attachments)
// is skipped by its size. Every length comes from untrusted data: elements and frames are bounded.
//
// Reads go through ByteReader (random access, blocking): a read that is interrupted (a seek is pending) or fails is
// reported as such, never as corrupt data. Not thread-safe (one decode thread).
#include <cstddef>
#include <climits>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace st::audio {

// Random-access bytes for the built-in demuxers.
class ByteReader {
public:
    enum class Status { Ok, End, Interrupted, Failed };
    virtual ~ByteReader() = default;
    virtual int64_t size() const = 0;
    // Exactly `n` bytes at `offset`; End when they pass the end of the data (nothing is copied then).
    virtual Status read(int64_t offset, void* dst, size_t n) = 0;
};

// Over a memory block (tests, fixtures).
class MemoryByteReader final : public ByteReader {
public:
    MemoryByteReader(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}
    int64_t size() const override { return static_cast<int64_t>(size_); }
    Status read(int64_t offset, void* dst, size_t n) override;

private:
    const uint8_t* data_;
    size_t size_;
};

namespace mkv {

enum class Codec : uint8_t { None, Opus, Vorbis, Other };

enum class Result {
    Ok,
    End,           // no more frames (end of the segment / data)
    Interrupted,   // a read was interrupted or cancelled (the caller retries or gives up)
    Failed,        // a read failed (network / file)
    Invalid,       // not Matroska / WebM, no audio track, or corrupt structure
};

struct AudioTrack {
    uint64_t number = 0;
    Codec codec = Codec::None;
    std::string codecId;              // "A_OPUS", "A_VORBIS", "A_AAC"...
    uint32_t sampleRate = 0;          // SamplingFrequency (Opus: the original input rate, decoding is at 48 kHz)
    uint32_t channels = 0;
    std::vector<uint8_t> codecPrivate;   // Opus: OpusHead; Vorbis: the three headers, Xiph-laced
    int64_t codecDelayNs = 0;         // Opus: the encoder's pre-skip
    int64_t seekPreRollNs = 0;        // Opus: decode this long before a seek target (80 ms)
};

struct Frame {
    static constexpr int64_t kContinued = INT64_MIN;
    std::vector<uint8_t> data;
    int64_t timeNs = kContinued;      // block timestamp (may be negative); kContinued for the 2nd.. frame of a laced
                                      // block (it follows the previous one)
    int64_t discardPaddingNs = 0;     // samples to drop at the end of this frame (the stream's last one)
};

class Demuxer {
public:
    Result open(ByteReader& reader);
    const AudioTrack& track() const { return track_; }
    int64_t durationNs() const { return durationNs_; }   // 0 = unknown

    Result next(Frame& out);
    // The next frame is the first of the cluster holding `ns` (the last one starting at or before it).
    Result seek(int64_t ns);
    size_t cueCount() const { return cues_.size(); }

private:
    struct Element {
        uint32_t id = 0;
        int64_t start = 0, content = 0;
        int64_t size = -1;            // -1 = unknown size
        int64_t end(int64_t limit) const { return size < 0 ? limit : content + size; }
    };
    Result header(int64_t pos, int64_t limit, Element& e);
    Result readBytes(int64_t pos, size_t n, std::vector<uint8_t>& out);
    Result readUint(const Element& e, uint64_t& v);
    Result readFloat(const Element& e, double& v);
    Result parseInfo(const Element& e);
    Result parseTracks(const Element& e);
    Result parseSeekHead(const Element& e);
    Result parseCues(int64_t pos);
    Result parseBlock(const Element& e, bool simple, int64_t discardPaddingNs);
    Result clusterTimeAt(int64_t pos, Element& cluster, int64_t& timeNs);

    ByteReader* r_ = nullptr;
    int64_t fileSize_ = 0;
    int64_t segData_ = 0, segEnd_ = 0;
    uint64_t scale_ = 1'000'000;      // ns per timestamp unit
    int64_t durationNs_ = 0;
    AudioTrack track_;
    int64_t firstCluster_ = -1;
    int64_t cuesPos_ = -1;            // absolute; -1 unknown
    bool cuesRead_ = false;
    std::vector<std::pair<int64_t, int64_t>> cues_;   // (time ns, absolute cluster position), sorted

    int64_t pos_ = 0;                 // next element to read
    bool inCluster_ = false;
    int64_t clusterEnd_ = 0;
    int64_t clusterTime_ = 0;         // timestamp units
    std::deque<Frame> pending_;       // laced frames not handed out yet
};

} // namespace mkv
} // namespace st::audio
