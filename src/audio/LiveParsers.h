#pragma once
// Pure parsers for live internet-radio streams (internal to st_audio, unit-tested by tests/liveaudio):
//   - elementary audio framing: MPEG audio (layer I/II/III), ADTS AAC and Ogg Opus -> one decodable frame at a
//     time, resynchronising after garbage, a reconnect or a splice;
//   - ICY (Shoutcast / Icecast) interleaved metadata and StreamTitle text (UTF-8, else the legacy code page);
//   - station playlists (.pls / .m3u / .asx) and HLS playlists (master + media);
//   - HLS segment containers: MPEG-2 TS, packed audio with ID3 and fragmented MP4 (AAC re-wrapped as ADTS).
// All sizes come from untrusted network data: every parser is bounded and never trusts a length field.
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::audio::live {

enum class Codec : uint8_t { None, Mpeg, Aac, Opus };

// One compressed frame: an MPEG audio frame, an ADTS frame (header included) or one Opus packet.
struct Frame {
    Codec codec = Codec::None;
    uint32_t sampleRate = 0;           // coded rate (HE-AAC: the core rate; the decoder may double it)
    uint32_t channels = 0;
    uint32_t samples = 0;              // per channel, at sampleRate
    uint32_t bitrate = 0;              // bits per second when the header says (MPEG), else 0
    uint8_t layer = 0;                 // MPEG audio layer 1..3
    uint8_t objectType = 0;            // AAC audio object type (2 = LC)
    std::vector<uint8_t> data;
    std::vector<uint8_t> codecPrivate; // Opus: the OpusHead packet
    std::optional<std::string> title;  // a new stream title (UTF-8) starts with this frame; "" = cleared
    bool discontinuity = false;        // audio was lost right before this frame (reconnect, dropped while paused)
};
int64_t frameDurationUs(const Frame& f);
// Decoder configuration identity: frames with the same key decode with the same decoder setup.
uint64_t frameConfigKey(const Frame& f);
const char* codecName(const Frame& f);  // "MP3", "MP2", "MP1", "AAC", "Opus"

// ---- elementary streams ---------------------------------------------------------------------------------------

struct MpegHeader {
    uint8_t version = 0;      // 1 = MPEG-1, 2 = MPEG-2, 3 = MPEG-2.5
    uint8_t layer = 0;        // 1..3
    uint8_t rateIndex = 0;
    uint32_t bitrate = 0;     // bits/s (free format is rejected)
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t samples = 0;     // per frame
    uint32_t length = 0;      // bytes, header included
};
std::optional<MpegHeader> parseMpegHeader(const uint8_t* p, size_t n);

struct AdtsHeader {
    uint8_t objectType = 0;   // profile + 1 (2 = LC)
    uint8_t freqIndex = 0;
    uint8_t channelConfig = 0;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t samples = 0;     // 1024 per raw data block
    uint32_t length = 0;      // bytes, header included
    uint32_t headerLength = 0;
};
std::optional<AdtsHeader> parseAdtsHeader(const uint8_t* p, size_t n);
// 7-byte ADTS header for a raw AAC frame of `payload` bytes (object types 1..4).
std::array<uint8_t, 7> makeAdtsHeader(uint8_t objectType, uint8_t freqIndex, uint8_t channelConfig, size_t payload);
uint32_t aacSampleRate(uint8_t freqIndex);  // 0 for invalid indexes

// ID3v2: total tag size when `p` starts a complete-looking tag header (10 bytes needed), else 0.
size_t id3TagSize(const uint8_t* p, size_t n);
// "Artist - Title" (or just one of them) from the TIT2 / TPE1 frames of an ID3v2 tag.
std::optional<std::string> id3Title(const uint8_t* tag, size_t n);

// MPEG audio / ADTS framing with resynchronisation. ID3v2 tags between frames are skipped (their title is kept).
class FrameParser {
public:
    void push(const uint8_t* data, size_t size);
    bool next(Frame& out);   // false: more bytes needed
    void reset();            // the next bytes do not continue the buffered ones
    Codec codec() const { return codec_; }
    // Bytes discarded while searching for a frame since the last one found (large = not an MPEG / AAC stream).
    size_t hunted() const { return hunted_; }
    size_t buffered() const { return buf_.size() - pos_; }
    std::optional<std::string> takeTitle();

private:
    enum class Try { Frame, NeedMore, Skip };
    Try tryAt(size_t at, Frame* out, size_t& consumed);
    void compact();

    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    Codec codec_ = Codec::None;
    uint32_t lockKey_ = 0;   // header fields the next frame must repeat
    bool locked_ = false;
    size_t hunted_ = 0;
    size_t skip_ = 0;        // bytes of an oversized ID3 tag still to drop
    std::optional<std::string> title_;
};

// Ogg pages -> Opus packets. Other Ogg codecs (Vorbis, FLAC, Speex) make it unsupported().
class OggDemuxer {
public:
    void push(const uint8_t* data, size_t size);
    bool next(Frame& out);
    bool unsupported() const { return !unsupportedCodec_.empty(); }
    const std::string& unsupportedCodec() const { return unsupportedCodec_; }
    size_t hunted() const { return hunted_; }
    std::optional<std::string> takeTitle();

private:
    bool readPage();
    void packet(std::vector<uint8_t>&& p, bool bos);

    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    std::vector<uint8_t> partial_;   // packet continued on the next page
    std::vector<Frame> ready_;
    size_t readyPos_ = 0;
    std::vector<uint8_t> head_;      // OpusHead of the current logical stream
    uint32_t channels_ = 0;
    int headerPackets_ = 0;          // OpusHead / OpusTags still expected
    std::string unsupportedCodec_;
    size_t hunted_ = 0;
    std::optional<std::string> title_;
};

// ---- ICY metadata ---------------------------------------------------------------------------------------------

// Splits an ICY body (icy-metaint > 0) into audio bytes and metadata blocks.
class IcyDemuxer {
public:
    explicit IcyDemuxer(size_t metaint) : metaint_(metaint), untilMeta_(metaint) {}
    void feed(const uint8_t* data, size_t size, const std::function<void(const uint8_t*, size_t)>& onAudio,
              const std::function<void(std::string_view)>& onMeta);

private:
    size_t metaint_;
    size_t untilMeta_;
    size_t metaLeft_ = 0;
    bool wantLength_ = false;
    std::string meta_;
};
// StreamTitle of an ICY metadata block (decoded to UTF-8, trimmed); nullopt if the block has none.
std::optional<std::string> icyStreamTitle(std::string_view metadata);
// UTF-8 when the bytes are valid UTF-8, else Windows-1254 (Turkish) on a Turkish system and Windows-1252 otherwise.
std::string decodeLegacyText(std::string_view bytes);

// ---- playlists ------------------------------------------------------------------------------------------------

enum class PlaylistType { None, Pls, M3u, Hls, Asx };
// By content type, then by the first bytes of the body, then by the URL's extension.
PlaylistType playlistType(std::string_view contentType, std::string_view head, std::string_view url);
// Absolute http(s) stream URLs of a .pls / .m3u / .asx playlist, in order.
std::vector<std::string> parsePlaylist(PlaylistType type, std::string_view body, const std::string& baseUrl);
std::string resolveUrl(const std::string& base, std::string_view ref);
bool isHttpUrl(std::string_view url);

struct HlsKey {
    std::string method;        // "" / "NONE" / "AES-128" / "SAMPLE-AES"...
    std::string uri;
    std::array<uint8_t, 16> iv{};
    bool hasIv = false;
};
struct HlsSegment {
    std::string uri;
    int64_t sequence = 0;
    double duration = 0;
    int64_t rangeOffset = -1;  // EXT-X-BYTERANGE
    int64_t rangeLength = -1;
    bool discontinuity = false;
    HlsKey key;
    std::string mapUri;        // EXT-X-MAP (fragmented MP4 init segment)
    int64_t mapOffset = -1, mapLength = -1;
    std::optional<std::string> title;   // EXTINF title="..." (artist="...") metadata
};
struct HlsVariant {
    std::string uri;
    int64_t bandwidth = 0;
    std::string codecs;
    std::string audioGroup;
};
struct HlsRendition {
    std::string groupId;
    std::string uri;
    bool isDefault = false;
};
struct HlsPlaylist {
    bool master = false;
    std::vector<HlsVariant> variants;
    std::vector<HlsRendition> audio;
    double targetDuration = 0;
    int64_t mediaSequence = 0;
    bool endList = false;
    std::vector<HlsSegment> segments;
};
HlsPlaylist parseHls(std::string_view body, const std::string& baseUrl);
// The media playlist to play from a master playlist: a variant (or its audio rendition) with codecs this engine
// decodes, the highest bandwidth up to 320 kbps (else the lowest). "" = nothing playable.
std::string pickHlsMedia(const HlsPlaylist& master);

// ---- HLS segment containers -----------------------------------------------------------------------------------

// MPEG-2 TS: audio elementary stream bytes (ADTS / MPEG audio) of the first supported audio stream. Every PMT is read:
// when it no longer lists the stream being played (an ad or another encoder after a discontinuity), its first
// supported audio stream takes over.
class TsDemuxer {
public:
    void feed(const uint8_t* data, size_t size, std::vector<uint8_t>& es);
    void endOfSegment();              // parses a pending ID3 metadata PES
    // The next bytes do not continue the previous ones (HLS discontinuity, skipped segment): a partial packet / PES
    // is dropped; the program tables stay until new ones arrive.
    void resync();
    Codec codec() const { return codec_; }
    bool unsupported() const { return unsupported_; }   // only audio this engine can't decode (AC-3, LATM...)
    std::optional<std::string> takeTitle();

private:
    void packet(const uint8_t* p, std::vector<uint8_t>& es);
    void table(const uint8_t* p, size_t n, bool pat);
    void flushId3();

    std::vector<uint8_t> carry_;
    int pmtPid_ = -1, audioPid_ = -1, id3Pid_ = -1;
    Codec codec_ = Codec::None;
    bool unsupported_ = false;
    bool audioStarted_ = false;
    std::vector<uint8_t> id3_;
    std::optional<std::string> title_;
};

// Fragmented MP4 (CMAF): AAC samples re-wrapped as ADTS, MP3 samples as they are.
class Fmp4Demuxer {
public:
    bool init(const uint8_t* data, size_t size);   // init segment: false if it has no supported audio track
    bool feed(const uint8_t* data, size_t size, std::vector<uint8_t>& es);   // moof + mdat pairs
    bool ready() const { return trackId_ != 0; }
    Codec codec() const { return codec_; }

private:
    uint32_t trackId_ = 0;
    Codec codec_ = Codec::None;
    uint32_t defaultSize_ = 0;       // trex default_sample_size
    uint8_t objectType_ = 0, freqIndex_ = 0, channelConfig_ = 0;
};

} // namespace st::audio::live
