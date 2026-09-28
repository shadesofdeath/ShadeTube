#include "audio/LiveParsers.h"

#include "core/Utf.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace st::audio::live {

namespace {

constexpr size_t kMaxId3Parse = 256 * 1024;     // larger ID3 tags are skipped unread
constexpr size_t kMaxPlaylistEntries = 64;
constexpr size_t kMaxId3Pes = 64 * 1024;
constexpr size_t kMaxOpusPacket = 64 * 1024;     // RFC 6716: at most 48 frames of 1275 bytes (120 ms at 510 kbps ~ 8 KB)

uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
uint32_t be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint32_t syncsafe(const uint8_t* p) { return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool istartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

size_t ifind(std::string_view s, std::string_view needle, size_t from = 0) {
    if (needle.empty() || s.size() < needle.size()) return std::string_view::npos;
    for (size_t i = from; i + needle.size() <= s.size(); ++i)
        if (iequals(s.substr(i, needle.size()), needle)) return i;
    return std::string_view::npos;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') s.remove_prefix(1);
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') s.remove_suffix(1);
    return s;
}

// Titles: control characters become spaces, runs of spaces collapse, the ends are trimmed.
std::string cleanText(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const bool space = static_cast<unsigned char>(c) < 0x20 || c == ' ';
        if (space) {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return std::string(trim(out));
}

std::vector<std::string_view> lines(std::string_view body) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) end = body.size();
        out.push_back(trim(body.substr(pos, end - pos)));
        pos = end + 1;
    }
    return out;
}

std::string_view stripBom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.remove_prefix(3);
    return s;
}

std::string utf16ToUtf8(const uint8_t* p, size_t n, bool bigEndian) {
    std::wstring w;
    w.reserve(n / 2);
    for (size_t i = 0; i + 1 < n; i += 2) {
        const wchar_t c = bigEndian ? static_cast<wchar_t>((p[i] << 8) | p[i + 1]) : static_cast<wchar_t>(p[i] | (p[i + 1] << 8));
        if (c == 0) break;
        w.push_back(c);
    }
    return toUtf8(w);
}

// ID3 text frame body: encoding byte + text (the first value when several are NUL-separated).
std::string id3Text(const uint8_t* p, size_t n) {
    if (n < 2) return {};
    const uint8_t enc = p[0];
    ++p;
    --n;
    if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) be = true, p += 2, n -= 2;
        else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) be = false, p += 2, n -= 2;
        return cleanText(utf16ToUtf8(p, n, be));
    }
    const auto* s = reinterpret_cast<const char*>(p);
    const size_t len = std::find(s, s + n, '\0') - s;
    return cleanText(enc == 3 ? std::string(s, len) : decodeLegacyText(std::string_view(s, len)));
}

std::string joinTitle(const std::string& artist, const std::string& title) {
    if (!artist.empty() && !title.empty()) return artist + " - " + title;
    return artist.empty() ? title : artist;
}

bool looksLikeAudio(std::string_view head) {
    if (head.size() >= 2 && static_cast<unsigned char>(head[0]) == 0xFF && (static_cast<unsigned char>(head[1]) & 0xE0) == 0xE0)
        return true;
    return head.starts_with("ID3") || head.starts_with("OggS") || head.starts_with("fLaC");
}

bool looksLikeText(std::string_view head) {
    size_t printable = 0;
    for (const char c : head) {
        const auto u = static_cast<unsigned char>(c);
        if (u == 0) return false;
        if (u >= 0x20 || u == '\r' || u == '\n' || u == '\t') ++printable;
    }
    return !head.empty() && printable * 10 >= head.size() * 9;
}

std::string extensionOf(std::string_view url) {
    url = url.substr(0, url.find_first_of("?#"));
    const size_t slash = url.rfind('/');
    const size_t dot = url.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string ext(url.substr(dot + 1));
    for (auto& c : ext) c = lower(c);
    return ext;
}

// HLS attribute list: KEY=VALUE,KEY="quoted, value",...
std::vector<std::pair<std::string, std::string>> parseAttributes(std::string_view s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) ++i;
        const size_t eq = s.find('=', i);
        if (eq == std::string_view::npos) break;
        std::string key(trim(s.substr(i, eq - i)));
        i = eq + 1;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            const size_t close = s.find('"', i + 1);
            const size_t end = close == std::string_view::npos ? s.size() : close;
            value = std::string(s.substr(i + 1, end - i - 1));
            i = end + 1;
        } else {
            size_t comma = s.find(',', i);
            if (comma == std::string_view::npos) comma = s.size();
            value = std::string(trim(s.substr(i, comma - i)));
            i = comma;
        }
        out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

std::string attribute(const std::vector<std::pair<std::string, std::string>>& attrs, std::string_view key) {
    for (const auto& [k, v] : attrs)
        if (iequals(k, key)) return v;
    return {};
}

void parseByteRange(std::string_view s, int64_t& offset, int64_t& length) {
    const size_t at = s.find('@');
    length = _atoi64(std::string(s.substr(0, at)).c_str());
    offset = at == std::string_view::npos ? -1 : _atoi64(std::string(s.substr(at + 1)).c_str());
    if (length <= 0) length = offset = -1;
}

bool parseHexIv(std::string_view s, std::array<uint8_t, 16>& iv) {
    if (s.size() < 3 || (s[0] != '0') || (s[1] != 'x' && s[1] != 'X')) return false;
    s.remove_prefix(2);
    if (s.size() > 32) return false;
    std::string hex(32 - s.size(), '0');
    hex.append(s);
    for (size_t i = 0; i < 16; ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            c = lower(c);
            return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        iv[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

// EXTINF metadata some radio HLS carry: `#EXTINF:10,title="Song",artist="Artist"`.
std::optional<std::string> extinfTitle(std::string_view rest) {
    if (ifind(rest, "title=\"") == std::string_view::npos) return std::nullopt;
    const auto attrs = parseAttributes(rest);
    return cleanText(joinTitle(decodeLegacyText(attribute(attrs, "artist")), decodeLegacyText(attribute(attrs, "title"))));
}

// Codecs of an HLS CODECS attribute this engine can decode (audio only; video codecs are ignored).
bool hlsCodecsPlayable(std::string_view codecs, bool& audioOnly) {
    audioOnly = true;
    if (codecs.empty()) return true;
    bool anyAudio = false, playable = true;
    size_t i = 0;
    while (i <= codecs.size()) {
        size_t comma = codecs.find(',', i);
        if (comma == std::string_view::npos) comma = codecs.size();
        const std::string_view c = trim(codecs.substr(i, comma - i));
        i = comma + 1;
        if (c.empty()) continue;
        if (istartsWith(c, "mp4a.")) {
            anyAudio = true;
            // mp4a.40.x (AAC LC / HE / HEv2), mp4a.40.34 / mp4a.69 / mp4a.6B (MP3)
            if (!istartsWith(c, "mp4a.40.") && !iequals(c, "mp4a.69") && !iequals(c, "mp4a.6b")) playable = false;
            else if (istartsWith(c, "mp4a.40.")) {
                const int aot = atoi(std::string(c.substr(8)).c_str());
                if (aot != 2 && aot != 5 && aot != 29 && aot != 34 && aot != 1) playable = false;
            }
        } else if (iequals(c, "mp3")) {
            anyAudio = true;
        } else if (istartsWith(c, "ac-3") || istartsWith(c, "ec-3") || istartsWith(c, "opus") ||
                   istartsWith(c, "flac") || istartsWith(c, "alac") || istartsWith(c, "mhm1")) {
            anyAudio = true;
            playable = false;
        } else {
            audioOnly = false;   // video (avc1, hvc1...) or text
        }
    }
    return anyAudio && playable;
}

// ---- Ogg CRC (polynomial 0x04C11DB7, no reflection) --------------------------------------------------------------

uint32_t oggCrc(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
            t[i] = r;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

uint32_t opusSamples(const std::vector<uint8_t>& p) {
    if (p.empty()) return 0;
    const uint8_t toc = p[0];
    const uint32_t config = toc >> 3;
    static const uint32_t kSilk[4] = {480, 960, 1920, 2880}, kCelt[4] = {120, 240, 480, 960};
    const uint32_t frame = config < 12 ? kSilk[config & 3] : config < 16 ? ((config & 1) ? 960 : 480) : kCelt[config & 3];
    uint32_t count = 1;
    switch (toc & 3) {
    case 0: count = 1; break;
    case 1:
    case 2: count = 2; break;
    default: count = p.size() >= 2 ? (p[1] & 0x3F) : 0; break;
    }
    return frame * count;
}

// ---- MP4 boxes ---------------------------------------------------------------------------------------------------

struct Box {
    uint32_t type = 0;
    const uint8_t* body = nullptr;
    size_t size = 0;      // body bytes
    size_t offset = 0;    // box start, relative to the buffer given to forEachBox
};

constexpr uint32_t fourcc(const char (&s)[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint8_t(s[3]);
}

template <class F>
void forEachBox(const uint8_t* p, size_t n, F&& fn) {
    size_t pos = 0;
    while (n - pos >= 8) {
        uint64_t size = be32(p + pos);
        const uint32_t type = be32(p + pos + 4);
        size_t header = 8;
        if (size == 1) {
            if (n - pos < 16) return;
            size = be64(p + pos + 8);
            header = 16;
        } else if (size == 0) {
            size = n - pos;
        }
        if (size < header || size > n - pos) return;
        if (!fn(Box{type, p + pos + header, static_cast<size_t>(size) - header, pos})) return;
        pos += static_cast<size_t>(size);
    }
}

std::optional<Box> findBox(const uint8_t* p, size_t n, uint32_t type) {
    std::optional<Box> found;
    forEachBox(p, n, [&](const Box& b) {
        if (b.type != type) return true;
        found = b;
        return false;
    });
    return found;
}

// MPEG-4 descriptor length (1..4 bytes of 7 bits).
bool descriptor(const uint8_t*& p, const uint8_t* end, uint8_t& tag, size_t& len) {
    if (p >= end) return false;
    tag = *p++;
    len = 0;
    for (int i = 0; i < 4; ++i) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len <= static_cast<size_t>(end - p);
}

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t bit = 0;
    uint32_t read(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            const size_t byte = bit >> 3;
            const uint32_t b = byte < n ? (p[byte] >> (7 - (bit & 7))) & 1 : 0;
            v = (v << 1) | b;
            ++bit;
        }
        return v;
    }
    bool ok() const { return (bit + 7) / 8 <= n; }
};

uint8_t freqIndexOf(uint32_t rate) {
    for (uint8_t i = 0; i < 13; ++i)
        if (aacSampleRate(i) == rate) return i;
    return 0xFF;
}

} // namespace

// ---- frames -------------------------------------------------------------------------------------------------------

int64_t frameDurationUs(const Frame& f) {
    return f.sampleRate ? static_cast<int64_t>(f.samples) * 1'000'000 / f.sampleRate : 0;
}

uint64_t frameConfigKey(const Frame& f) {
    uint64_t key = static_cast<uint64_t>(f.codec) | (uint64_t(f.layer) << 8) | (uint64_t(f.objectType) << 16) |
                   (uint64_t(f.channels & 0xFF) << 24) | (uint64_t(f.sampleRate) << 32);
    if (f.codec == Codec::Opus) {   // the OpusHead (channel mapping) configures the decoder
        for (const uint8_t b : f.codecPrivate) key = (key ^ b) * 1099511628211ull;
    }
    return key;
}

const char* codecName(const Frame& f) {
    switch (f.codec) {
    case Codec::Mpeg: return f.layer == 3 ? "MP3" : f.layer == 2 ? "MP2" : "MP1";
    case Codec::Aac: return "AAC";
    case Codec::Opus: return "Opus";
    case Codec::None: break;
    }
    return "";
}

std::optional<MpegHeader> parseMpegHeader(const uint8_t* p, size_t n) {
    if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return std::nullopt;
    const int ver = (p[1] >> 3) & 3;   // 0: MPEG-2.5, 1: reserved, 2: MPEG-2, 3: MPEG-1
    const int lay = (p[1] >> 1) & 3;   // 1: layer III, 2: layer II, 3: layer I, 0: reserved
    const int bri = p[2] >> 4, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (ver == 1 || lay == 0 || bri == 0 || bri == 15 || sri == 3 || (p[3] & 3) == 2) return std::nullopt;
    static const uint16_t kBitrates[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    static const uint32_t kRates[3] = {44100, 48000, 32000};
    MpegHeader h;
    h.version = static_cast<uint8_t>(ver == 3 ? 1 : ver == 2 ? 2 : 3);
    h.layer = static_cast<uint8_t>(4 - lay);
    h.rateIndex = static_cast<uint8_t>(sri);
    h.sampleRate = kRates[sri] >> (h.version - 1);   // MPEG-2 halves the rates, MPEG-2.5 quarters them
    h.bitrate = kBitrates[h.version == 1 ? 0 : 1][h.layer - 1][bri] * 1000u;
    h.channels = (p[3] >> 6) == 3 ? 1 : 2;
    if (h.layer == 1) {
        h.samples = 384;
        h.length = (12 * h.bitrate / h.sampleRate + pad) * 4;
    } else if (h.layer == 2) {
        h.samples = 1152;
        h.length = 144 * h.bitrate / h.sampleRate + pad;
    } else {
        h.samples = h.version == 1 ? 1152 : 576;
        h.length = (h.version == 1 ? 144 : 72) * h.bitrate / h.sampleRate + pad;
    }
    if (h.length < 24) return std::nullopt;
    return h;
}

uint32_t aacSampleRate(uint8_t freqIndex) {
    static const uint32_t kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    return freqIndex < 13 ? kRates[freqIndex] : 0;
}

std::optional<AdtsHeader> parseAdtsHeader(const uint8_t* p, size_t n) {
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return std::nullopt;   // 12-bit sync, layer 00
    AdtsHeader h;
    const bool crc = (p[1] & 1) == 0;
    h.objectType = static_cast<uint8_t>((p[2] >> 6) + 1);
    h.freqIndex = static_cast<uint8_t>((p[2] >> 2) & 0xF);
    h.channelConfig = static_cast<uint8_t>(((p[2] & 1) << 2) | (p[3] >> 6));
    h.length = ((p[3] & 3u) << 11) | (uint32_t(p[4]) << 3) | (p[5] >> 5);
    h.headerLength = crc ? 9 : 7;
    h.sampleRate = aacSampleRate(h.freqIndex);
    h.channels = h.channelConfig == 0 ? 2 : h.channelConfig == 7 ? 8 : h.channelConfig;
    h.samples = 1024 * ((p[6] & 3u) + 1);
    if (!h.sampleRate || h.length <= h.headerLength) return std::nullopt;
    return h;
}

std::array<uint8_t, 7> makeAdtsHeader(uint8_t objectType, uint8_t freqIndex, uint8_t channelConfig, size_t payload) {
    const size_t len = payload + 7;
    return {0xFF,
            0xF1,   // MPEG-4, layer 0, no CRC
            static_cast<uint8_t>((((objectType - 1) & 3) << 6) | ((freqIndex & 0xF) << 2) | ((channelConfig >> 2) & 1)),
            static_cast<uint8_t>(((channelConfig & 3) << 6) | ((len >> 11) & 3)),
            static_cast<uint8_t>((len >> 3) & 0xFF),
            static_cast<uint8_t>(((len & 7) << 5) | 0x1F),   // buffer fullness 0x7FF (VBR)
            0xFC};                                           // ... one raw data block
}

size_t id3TagSize(const uint8_t* p, size_t n) {
    if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3' || p[3] == 0xFF || p[4] == 0xFF) return 0;
    if ((p[6] | p[7] | p[8] | p[9]) & 0x80) return 0;
    size_t size = size_t(syncsafe(p + 6)) + 10;
    if (p[5] & 0x10) size += 10;   // footer (v2.4)
    return size;
}

std::optional<std::string> id3Title(const uint8_t* tag, size_t n) {
    const size_t total = id3TagSize(tag, n);
    if (!total) return std::nullopt;
    const int ver = tag[3];
    const size_t end = std::min(n, total - ((tag[5] & 0x10) ? 10 : 0));
    size_t pos = 10;
    if ((tag[5] & 0x40) && ver >= 3) {   // extended header
        if (pos + 4 > end) return std::nullopt;
        pos += ver == 4 ? syncsafe(tag + pos) : be32(tag + pos) + 4;
    }
    const size_t idLen = ver == 2 ? 3 : 4, headerLen = ver == 2 ? 6 : 10;
    std::string title, artist;
    while (pos + headerLen <= end) {
        const uint8_t* f = tag + pos;
        if (f[0] == 0) break;   // padding
        const size_t size = ver == 2 ? be24(f + 3) : ver == 4 ? syncsafe(f + 4) : be32(f + 4);
        pos += headerLen;
        if (size > end - pos) break;
        const std::string_view id(reinterpret_cast<const char*>(f), idLen);
        if (id == "TIT2" || id == "TT2") title = id3Text(tag + pos, size);
        else if (id == "TPE1" || id == "TP1") artist = id3Text(tag + pos, size);
        pos += size;
    }
    if (title.empty() && artist.empty()) return std::nullopt;
    return joinTitle(artist, title);
}

// ---- FrameParser --------------------------------------------------------------------------------------------------

void FrameParser::compact() {
    if (pos_ > 0 && (pos_ >= buf_.size() || pos_ >= 64 * 1024 || pos_ * 2 >= buf_.size())) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(pos_));
        pos_ = 0;
    }
}

void FrameParser::push(const uint8_t* data, size_t size) {
    compact();
    buf_.insert(buf_.end(), data, data + size);
}

void FrameParser::reset() {
    buf_.clear();
    pos_ = 0;
    locked_ = false;
    hunted_ = 0;
    skip_ = 0;
}

std::optional<std::string> FrameParser::takeTitle() {
    auto t = std::move(title_);
    title_.reset();
    return t;
}

FrameParser::Try FrameParser::tryAt(size_t at, Frame* out, size_t& consumed) {
    const uint8_t* p = buf_.data() + at;
    const size_t avail = buf_.size() - at;
    const bool adts = avail >= 2 && (p[1] & 0xF6) == 0xF0;
    Frame f;
    uint32_t key = 0, length = 0;
    size_t nextHeader = 0;   // bytes of the following header needed to confirm an unlocked candidate
    if (adts) {
        if (avail < 9) return Try::NeedMore;
        const auto h = parseAdtsHeader(p, avail);
        if (!h) return Try::Skip;
        key = 0x1000000u | (uint32_t(h->objectType) << 16) | (uint32_t(h->freqIndex) << 8) | h->channelConfig;
        length = h->length;
        nextHeader = 7;
        f.codec = Codec::Aac;
        f.sampleRate = h->sampleRate;
        f.channels = h->channels;
        f.samples = h->samples;
        f.objectType = h->objectType;
    } else {
        const auto h = parseMpegHeader(p, avail);
        if (!h) return avail < 4 ? Try::NeedMore : Try::Skip;
        key = 0x2000000u | (uint32_t(h->version) << 16) | (uint32_t(h->layer) << 8) | (uint32_t(h->rateIndex) << 4) |
              (h->channels == 1 ? 1u : 0u);
        length = h->length;
        nextHeader = 4;
        f.codec = Codec::Mpeg;
        f.sampleRate = h->sampleRate;
        f.channels = h->channels;
        f.samples = h->samples;
        f.bitrate = h->bitrate;
        f.layer = h->layer;
    }
    const bool confirmed = locked_ && key == lockKey_;
    if (!confirmed) {
        // A candidate outside the locked stream: the next frame must start right after it with the same parameters.
        if (avail < length + nextHeader) return Try::NeedMore;
        const uint8_t* q = p + length;
        uint32_t nextKey = 0;
        if (adts) {
            if (const auto h2 = parseAdtsHeader(q, avail - length))
                nextKey = 0x1000000u | (uint32_t(h2->objectType) << 16) | (uint32_t(h2->freqIndex) << 8) | h2->channelConfig;
        } else if (const auto h2 = parseMpegHeader(q, avail - length)) {
            nextKey = 0x2000000u | (uint32_t(h2->version) << 16) | (uint32_t(h2->layer) << 8) | (uint32_t(h2->rateIndex) << 4) |
                      (h2->channels == 1 ? 1u : 0u);
        }
        if (nextKey != key) return Try::Skip;
    } else if (avail < length) {
        return Try::NeedMore;
    }
    locked_ = true;
    lockKey_ = key;
    codec_ = f.codec;
    hunted_ = 0;
    f.data.assign(p, p + length);
    consumed = length;
    *out = std::move(f);
    return Try::Frame;
}

bool FrameParser::next(Frame& out) {
    for (;;) {
        if (skip_ > 0) {   // inside an oversized ID3 tag
            const size_t k = std::min(skip_, buf_.size() - pos_);
            pos_ += k;
            skip_ -= k;
            if (skip_ > 0) return false;
        }
        const size_t avail = buf_.size() - pos_;
        if (avail < 4) return false;
        const uint8_t* p = buf_.data() + pos_;
        if (p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
            if (avail < 10) return false;
            if (const size_t tag = id3TagSize(p, avail)) {
                if (tag > kMaxId3Parse) {
                    skip_ = tag;
                    continue;
                }
                if (avail < tag) return false;
                if (auto t = id3Title(p, tag)) title_ = std::move(t);
                pos_ += tag;
                continue;
            }
        }
        if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) {
            size_t consumed = 0;
            switch (tryAt(pos_, &out, consumed)) {
            case Try::Frame: pos_ += consumed; return true;
            case Try::NeedMore: return false;
            case Try::Skip: break;
            }
        }
        // Not a frame here: lose the lock and hunt for the next possible start.
        locked_ = false;
        size_t i = pos_ + 1;
        while (i < buf_.size() && buf_[i] != 0xFF && buf_[i] != 'I') ++i;
        hunted_ += i - pos_;
        pos_ = i;
        compact();
    }
}

// ---- OggDemuxer ---------------------------------------------------------------------------------------------------

void OggDemuxer::push(const uint8_t* data, size_t size) {
    if (pos_ > 0 && (pos_ >= 64 * 1024 || pos_ * 2 >= buf_.size())) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data, data + size);
}

std::optional<std::string> OggDemuxer::takeTitle() {
    auto t = std::move(title_);
    title_.reset();
    return t;
}

bool OggDemuxer::next(Frame& out) {
    for (;;) {
        if (readyPos_ < ready_.size()) {
            out = std::move(ready_[readyPos_++]);
            return true;
        }
        ready_.clear();
        readyPos_ = 0;
        if (unsupported() || !readPage()) return false;
    }
}

bool OggDemuxer::readPage() {
    for (;;) {
        const size_t avail = buf_.size() - pos_;
        if (avail < 27) return false;
        const uint8_t* p = buf_.data() + pos_;
        if (std::memcmp(p, "OggS", 4) != 0 || p[4] != 0) {
            ++pos_;
            ++hunted_;
            continue;
        }
        const size_t segments = p[26];
        if (avail < 27 + segments) return false;
        size_t body = 0;
        for (size_t i = 0; i < segments; ++i) body += p[27 + i];
        const size_t pageSize = 27 + segments + body;
        if (avail < pageSize) return false;
        // Verify the CRC (with its own field zeroed) so garbage that happens to contain "OggS" is not taken as a page.
        std::vector<uint8_t> page(p, p + pageSize);
        const uint32_t crc = le32(page.data() + 22);
        std::memset(page.data() + 22, 0, 4);
        if (oggCrc(page.data(), page.size()) != crc) {
            ++pos_;
            ++hunted_;
            continue;
        }
        hunted_ = 0;
        const uint8_t flags = p[5];
        const bool bos = (flags & 0x02) != 0;
        if (bos) {   // a new logical stream (chained Ogg: Icecast starts one per metadata change)
            partial_.clear();
            head_.clear();
            headerPackets_ = 0;
        }
        const bool continued = (flags & 0x01) != 0;
        // A continued page whose packet start was never seen (joined mid-stream): drop that packet's tail. An
        // unfinished packet followed by a fresh page is broken: drop it.
        bool skipFirst = continued && partial_.empty();
        if (!continued) partial_.clear();
        const uint8_t* data = p + 27 + segments;
        size_t off = 0;
        bool firstPacket = true;
        for (size_t i = 0; i < segments; ++i) {
            const size_t len = p[27 + i];
            if (!skipFirst) partial_.insert(partial_.end(), data + off, data + off + len);
            off += len;
            if (len < 255) {
                if (!skipFirst) packet(std::move(partial_), bos && firstPacket);
                partial_.clear();
                skipFirst = false;
                firstPacket = false;
            }
        }
        if (partial_.size() > 1024 * 1024) partial_.clear();   // runaway packet
        pos_ += pageSize;
        return true;
    }
}

void OggDemuxer::packet(std::vector<uint8_t>&& p, bool bos) {
    auto startsWith = [&](const char* s, size_t n) { return p.size() >= n && std::memcmp(p.data(), s, n) == 0; };
    if (bos || head_.empty()) {
        if (startsWith("OpusHead", 8) && p.size() >= 19) {
            head_ = p;
            channels_ = std::max<uint32_t>(1, p[9]);
            headerPackets_ = 1;   // OpusTags follows
        } else if (startsWith("\x01vorbis", 7)) {
            unsupportedCodec_ = "Vorbis";
        } else if (startsWith("\x7F" "FLAC", 5)) {
            unsupportedCodec_ = "FLAC";
        } else if (startsWith("Speex", 5)) {
            unsupportedCodec_ = "Speex";
        }
        return;   // other streams (Skeleton...) are ignored
    }
    if (headerPackets_ > 0) {
        headerPackets_ = 0;
        if (!startsWith("OpusTags", 8) || p.size() < 16) return;
        // Vorbis comments: vendor string, then "KEY=value" entries.
        size_t pos = 8;
        const uint32_t vendor = le32(p.data() + pos);
        pos += 4;
        if (vendor > p.size() - pos) return;
        pos += vendor;
        if (p.size() - pos < 4) return;
        uint32_t count = le32(p.data() + pos);
        pos += 4;
        std::string title, artist;
        while (count-- > 0 && p.size() - pos >= 4) {
            const uint32_t len = le32(p.data() + pos);
            pos += 4;
            if (len > p.size() - pos) break;
            const std::string_view entry(reinterpret_cast<const char*>(p.data() + pos), len);
            pos += len;
            if (istartsWith(entry, "TITLE=")) title = cleanText(entry.substr(6));
            else if (istartsWith(entry, "ARTIST=")) artist = cleanText(entry.substr(7));
        }
        if (!title.empty() || !artist.empty()) title_ = joinTitle(artist, title);
        return;
    }
    Frame f;
    f.codec = Codec::Opus;
    f.sampleRate = 48000;
    f.channels = channels_;
    f.samples = opusSamples(p);
    f.data = std::move(p);
    f.codecPrivate = head_;
    if (f.samples > 0 && f.data.size() <= kMaxOpusPacket) ready_.push_back(std::move(f));   // larger: not Opus audio
}

// ---- ICY ----------------------------------------------------------------------------------------------------------

void IcyDemuxer::feed(const uint8_t* data, size_t size, const std::function<void(const uint8_t*, size_t)>& onAudio,
                      const std::function<void(std::string_view)>& onMeta) {
    if (metaint_ == 0) {
        if (size) onAudio(data, size);
        return;
    }
    size_t i = 0;
    while (i < size) {
        if (metaLeft_ > 0) {
            const size_t k = std::min(metaLeft_, size - i);
            meta_.append(reinterpret_cast<const char*>(data + i), k);
            i += k;
            metaLeft_ -= k;
            if (metaLeft_ == 0) {
                onMeta(meta_);
                meta_.clear();
                untilMeta_ = metaint_;
            }
            continue;
        }
        if (wantLength_) {
            wantLength_ = false;
            metaLeft_ = size_t(data[i++]) * 16;
            if (metaLeft_ == 0) untilMeta_ = metaint_;
            continue;
        }
        const size_t k = std::min(untilMeta_, size - i);
        if (k) onAudio(data + i, k);
        i += k;
        untilMeta_ -= k;
        if (untilMeta_ == 0) wantLength_ = true;
    }
}

std::optional<std::string> icyStreamTitle(std::string_view meta) {
    meta = meta.substr(0, meta.find('\0'));
    const size_t key = ifind(meta, "StreamTitle='");
    if (key == std::string_view::npos) return std::nullopt;
    const size_t start = key + 13;
    // The title may contain quotes: it ends at the first "';" followed by the end of the block or by the next key
    // ("StreamUrl=", "json=", ...), else at the last quote.
    size_t end = std::string_view::npos;
    for (size_t p = meta.find("';", start); p != std::string_view::npos; p = meta.find("';", p + 1)) {
        size_t k = p + 2;
        while (k < meta.size() && (isalnum(static_cast<unsigned char>(meta[k])) || meta[k] == '_')) ++k;
        if (trim(meta.substr(p + 2)).empty() || (k > p + 2 && k < meta.size() && meta[k] == '=')) {
            end = p;
            break;
        }
    }
    if (end == std::string_view::npos) {
        end = meta.rfind('\'');
        if (end == std::string_view::npos || end < start) end = meta.size();
    }
    return cleanText(decodeLegacyText(meta.substr(start, end - start)));
}

std::string decodeLegacyText(std::string_view bytes) {
    if (bytes.empty()) return {};
    const int len = static_cast<int>(std::min<size_t>(bytes.size(), 1 << 20));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), len, nullptr, 0) > 0) return std::string(bytes.substr(0, len));
    const UINT cp = GetACP() == 1254 ? 1254 : 1252;
    const int n = MultiByteToWideChar(cp, 0, bytes.data(), len, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), len, w.data(), n);
    return toUtf8(w);
}

// ---- playlists ----------------------------------------------------------------------------------------------------

bool isHttpUrl(std::string_view url) { return istartsWith(url, "http://") || istartsWith(url, "https://"); }

std::string resolveUrl(const std::string& base, std::string_view ref) {
    ref = trim(ref);
    if (ref.empty()) return {};
    const size_t colon = ref.find(':');
    const size_t firstSep = ref.find_first_of("/?#");
    if (colon != std::string_view::npos && (firstSep == std::string_view::npos || colon < firstSep)) return std::string(ref);   // absolute
    const size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return std::string(ref);
    if (ref.starts_with("//")) return base.substr(0, schemeEnd + 1) + std::string(ref);
    const size_t pathStart = base.find('/', schemeEnd + 3);
    const std::string origin = pathStart == std::string::npos ? base.substr(0, base.find_first_of("?#", schemeEnd + 3)) : base.substr(0, pathStart);
    std::string path;
    if (ref.starts_with("/")) {
        path = std::string(ref);
    } else {
        std::string basePath = pathStart == std::string::npos ? "/" : base.substr(pathStart);
        basePath = basePath.substr(0, basePath.find_first_of("?#"));
        if (ref.starts_with("?")) return origin + basePath + std::string(ref);
        path = basePath.substr(0, basePath.rfind('/') + 1) + std::string(ref);
    }
    // Remove dot segments of the path (the query is left alone).
    const size_t q = path.find_first_of("?#");
    const std::string query = q == std::string::npos ? "" : path.substr(q);
    std::vector<std::string> parts;
    std::string_view rest = std::string_view(path).substr(0, q == std::string::npos ? path.size() : q);
    const bool trailing = rest.ends_with("/") || rest.ends_with("/.") || rest.ends_with("/..");
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        const std::string_view seg = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.emplace_back(seg);
    }
    std::string out = origin;
    for (const auto& s : parts) out += "/" + s;
    if (parts.empty() || trailing) out += "/";
    return out + query;
}

PlaylistType playlistType(std::string_view contentType, std::string_view head, std::string_view url) {
    const std::string_view h = trim(stripBom(head));
    if (h.starts_with("#EXTM3U")) return h.find("#EXT-X-") != std::string_view::npos ? PlaylistType::Hls : PlaylistType::M3u;
    if (istartsWith(h, "[playlist]")) return PlaylistType::Pls;
    if (istartsWith(h, "<asx")) return PlaylistType::Asx;
    if (looksLikeAudio(head)) return PlaylistType::None;
    std::string_view ct = trim(contentType.substr(0, contentType.find(';')));
    const bool text = looksLikeText(head);
    if (iequals(ct, "audio/x-scpls") || iequals(ct, "application/pls+xml") || iequals(ct, "audio/scpls")) return PlaylistType::Pls;
    if (iequals(ct, "application/vnd.apple.mpegurl") || iequals(ct, "application/x-mpegurl") ||
        iequals(ct, "audio/x-mpegurl") || iequals(ct, "audio/mpegurl") || iequals(ct, "vnd.apple.mpegurl"))
        return h.find("#EXT-X-") != std::string_view::npos ? PlaylistType::Hls : PlaylistType::M3u;
    if ((iequals(ct, "video/x-ms-asf") || iequals(ct, "video/x-ms-asx") || iequals(ct, "audio/x-ms-wax") ||
         iequals(ct, "application/x-ms-asx")) && h.starts_with("<"))
        return PlaylistType::Asx;
    if (!text) return PlaylistType::None;
    const std::string ext = extensionOf(url);
    if (ext == "pls") return PlaylistType::Pls;
    if (ext == "m3u" || ext == "m3u8") return h.find("#EXT-X-") != std::string_view::npos ? PlaylistType::Hls : PlaylistType::M3u;
    if (ext == "asx" || ext == "wax") return PlaylistType::Asx;
    // A plain list of stream URLs served as text.
    if (istartsWith(ct, "text/") && (istartsWith(h, "http://") || istartsWith(h, "https://"))) return PlaylistType::M3u;
    return PlaylistType::None;
}

std::vector<std::string> parsePlaylist(PlaylistType type, std::string_view body, const std::string& baseUrl) {
    std::vector<std::string> out;
    auto add = [&](std::string_view ref) {
        std::string url = resolveUrl(baseUrl, ref);
        if (isHttpUrl(url) && out.size() < kMaxPlaylistEntries && std::find(out.begin(), out.end(), url) == out.end())
            out.push_back(std::move(url));
    };
    body = stripBom(body);
    switch (type) {
    case PlaylistType::Pls:
        for (const auto line : lines(body)) {
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos && istartsWith(line, "file")) add(line.substr(eq + 1));
        }
        break;
    case PlaylistType::M3u:
        for (const auto line : lines(body))
            if (!line.empty() && line[0] != '#') add(line);
        break;
    case PlaylistType::Asx: {
        for (const std::string_view tag : {std::string_view("<ref"), std::string_view("<entryref")}) {
            size_t pos = 0;
            while ((pos = ifind(body, tag, pos)) != std::string_view::npos) {
                const size_t close = body.find('>', pos);
                const std::string_view element = body.substr(pos, close == std::string_view::npos ? body.size() - pos : close - pos);
                pos += tag.size();
                const size_t href = ifind(element, "href");
                if (href == std::string_view::npos) continue;
                size_t q = element.find_first_of("\"'", href);
                if (q == std::string_view::npos) continue;
                const size_t endq = element.find(element[q], q + 1);
                if (endq == std::string_view::npos) continue;
                std::string url(element.substr(q + 1, endq - q - 1));
                for (size_t amp; (amp = url.find("&amp;")) != std::string::npos;) url.replace(amp, 5, "&");
                add(url);
            }
        }
        break;
    }
    case PlaylistType::Hls:
    case PlaylistType::None: break;
    }
    return out;
}

HlsPlaylist parseHls(std::string_view body, const std::string& baseUrl) {
    HlsPlaylist pl;
    std::optional<HlsVariant> pendingVariant;
    double duration = 0;
    int64_t rangeOffset = -1, rangeLength = -1, nextRangeOffset = 0;
    bool discontinuity = false;
    std::optional<std::string> title;
    HlsKey key;
    std::string mapUri;
    int64_t mapOffset = -1, mapLength = -1;
    int64_t index = 0;
    std::string lastUri;
    for (const auto line : lines(stripBom(body))) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.starts_with("#EXT-X-STREAM-INF:")) {
                const auto attrs = parseAttributes(line.substr(18));
                HlsVariant v;
                v.bandwidth = _atoi64(attribute(attrs, "BANDWIDTH").c_str());
                v.codecs = attribute(attrs, "CODECS");
                v.audioGroup = attribute(attrs, "AUDIO");
                pendingVariant = std::move(v);
            } else if (line.starts_with("#EXT-X-MEDIA:")) {
                const auto attrs = parseAttributes(line.substr(13));
                if (iequals(attribute(attrs, "TYPE"), "AUDIO")) {
                    HlsRendition r;
                    r.groupId = attribute(attrs, "GROUP-ID");
                    const std::string uri = attribute(attrs, "URI");
                    r.uri = uri.empty() ? std::string() : resolveUrl(baseUrl, uri);
                    r.isDefault = iequals(attribute(attrs, "DEFAULT"), "YES");
                    if (pl.audio.size() < 64) pl.audio.push_back(std::move(r));
                }
            } else if (line.starts_with("#EXT-X-TARGETDURATION:")) {
                pl.targetDuration = atof(std::string(line.substr(22)).c_str());
            } else if (line.starts_with("#EXT-X-MEDIA-SEQUENCE:")) {
                pl.mediaSequence = _atoi64(std::string(line.substr(22)).c_str());
            } else if (line.starts_with("#EXT-X-ENDLIST")) {
                pl.endList = true;
            } else if (line.starts_with("#EXTINF:")) {
                const std::string_view rest = line.substr(8);
                const size_t comma = rest.find(',');
                duration = atof(std::string(rest.substr(0, comma)).c_str());
                title = comma == std::string_view::npos ? std::nullopt : extinfTitle(rest.substr(comma + 1));
            } else if (line.starts_with("#EXT-X-BYTERANGE:")) {
                parseByteRange(line.substr(17), rangeOffset, rangeLength);
            } else if (line.starts_with("#EXT-X-DISCONTINUITY") && !line.starts_with("#EXT-X-DISCONTINUITY-SEQUENCE")) {
                discontinuity = true;
            } else if (line.starts_with("#EXT-X-KEY:")) {
                const auto attrs = parseAttributes(line.substr(11));
                key = {};
                key.method = attribute(attrs, "METHOD");
                if (const std::string uri = attribute(attrs, "URI"); !uri.empty()) key.uri = resolveUrl(baseUrl, uri);
                key.hasIv = parseHexIv(attribute(attrs, "IV"), key.iv);
            } else if (line.starts_with("#EXT-X-MAP:")) {
                const auto attrs = parseAttributes(line.substr(11));
                mapUri = resolveUrl(baseUrl, attribute(attrs, "URI"));
                mapOffset = mapLength = -1;
                if (const std::string br = attribute(attrs, "BYTERANGE"); !br.empty()) {
                    parseByteRange(br, mapOffset, mapLength);
                    if (mapLength > 0 && mapOffset < 0) mapOffset = 0;
                }
            }
            continue;
        }
        const std::string uri = resolveUrl(baseUrl, line);
        if (pendingVariant) {
            pendingVariant->uri = uri;
            if (pl.variants.size() < 64) pl.variants.push_back(std::move(*pendingVariant));
            pendingVariant.reset();
            continue;
        }
        HlsSegment s;
        s.uri = uri;
        s.sequence = pl.mediaSequence + index++;
        s.duration = duration;
        if (rangeLength > 0) {
            s.rangeLength = rangeLength;
            s.rangeOffset = rangeOffset >= 0 ? rangeOffset : (uri == lastUri ? nextRangeOffset : 0);
            nextRangeOffset = s.rangeOffset + s.rangeLength;
        }
        s.discontinuity = discontinuity;
        s.key = key;
        s.mapUri = mapUri;
        s.mapOffset = mapOffset;
        s.mapLength = mapLength;
        s.title = std::move(title);
        if (pl.segments.size() < 10000) pl.segments.push_back(std::move(s));
        lastUri = uri;
        duration = 0;
        rangeOffset = rangeLength = -1;
        discontinuity = false;
        title.reset();
    }
    pl.master = !pl.variants.empty();
    return pl;
}

std::string pickHlsMedia(const HlsPlaylist& master) {
    const HlsVariant* best = nullptr;
    bool bestAudioOnly = false;
    for (const auto& v : master.variants) {
        bool audioOnly = true;
        if (!hlsCodecsPlayable(v.codecs, audioOnly) || v.uri.empty()) continue;
        if (!best) {
            best = &v;
            bestAudioOnly = audioOnly;
            continue;
        }
        // Audio-only variants first; then the highest bandwidth up to 320 kbps, else the lowest above it.
        if (audioOnly != bestAudioOnly) {
            if (audioOnly) best = &v, bestAudioOnly = true;
            continue;
        }
        const bool inRange = v.bandwidth <= 320000, bestInRange = best->bandwidth <= 320000;
        if (inRange != bestInRange ? inRange : inRange ? v.bandwidth > best->bandwidth : v.bandwidth < best->bandwidth) best = &v;
    }
    if (!best) return {};
    if (!best->audioGroup.empty()) {
        const HlsRendition* pick = nullptr;
        for (const auto& r : master.audio)
            if (r.groupId == best->audioGroup && !r.uri.empty() && (!pick || r.isDefault)) pick = &r;
        if (pick) return pick->uri;
    }
    return best->uri;
}

// ---- TsDemuxer ----------------------------------------------------------------------------------------------------

std::optional<std::string> TsDemuxer::takeTitle() {
    auto t = std::move(title_);
    title_.reset();
    return t;
}

void TsDemuxer::feed(const uint8_t* data, size_t size, std::vector<uint8_t>& es) {
    carry_.insert(carry_.end(), data, data + size);
    size_t i = 0;
    while (carry_.size() - i >= 188) {
        if (carry_[i] != 0x47 || (carry_.size() - i >= 189 && carry_[i + 188] != 0x47 && carry_.size() - i >= 376)) {
            ++i;   // resynchronise on the packet grid
            continue;
        }
        packet(carry_.data() + i, es);
        i += 188;
    }
    carry_.erase(carry_.begin(), carry_.begin() + static_cast<ptrdiff_t>(i));
}

void TsDemuxer::endOfSegment() { flushId3(); }

void TsDemuxer::resync() {
    carry_.clear();
    audioStarted_ = false;
    id3_.clear();
}

void TsDemuxer::flushId3() {
    if (id3_.empty()) return;
    if (auto t = id3Title(id3_.data(), id3_.size())) title_ = std::move(t);
    id3_.clear();
}

void TsDemuxer::packet(const uint8_t* p, std::vector<uint8_t>& es) {
    if (p[1] & 0x80) return;   // transport error
    const bool start = (p[1] & 0x40) != 0;
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    const int afc = (p[3] >> 4) & 3;
    if (afc == 0 || afc == 2) return;   // no payload
    size_t off = 4;
    if (afc == 3) off += 1 + size_t(p[4]);
    if (off >= 188) return;
    const uint8_t* pl = p + off;
    size_t n = 188 - off;
    if (pid == 0) {
        if (start) table(pl, n, true);
        return;
    }
    if (pid == pmtPid_) {
        if (start) table(pl, n, false);
        return;
    }
    auto skipPesHeader = [&]() -> bool {
        if (n < 9 || pl[0] != 0 || pl[1] != 0 || pl[2] != 1) return false;
        const size_t h = 9 + size_t(pl[8]);
        if (h > n) return false;
        pl += h;
        n -= h;
        return true;
    };
    if (pid == audioPid_) {
        if (start) audioStarted_ = skipPesHeader();
        if (audioStarted_) es.insert(es.end(), pl, pl + n);
    } else if (pid == id3Pid_) {
        if (start) {
            flushId3();
            if (!skipPesHeader()) return;
            id3_.assign(pl, pl + n);
        } else if (!id3_.empty() && id3_.size() + n <= kMaxId3Pes) {
            id3_.insert(id3_.end(), pl, pl + n);
        }
    }
}

void TsDemuxer::table(const uint8_t* p, size_t n, bool pat) {
    if (n < 1) return;
    const size_t pointer = p[0];
    if (1 + pointer >= n) return;
    p += 1 + pointer;
    n -= 1 + pointer;
    if (n < 12) return;
    const size_t sectionLength = ((p[1] & 0x0F) << 8) | p[2];
    const size_t end = std::min(n, 3 + sectionLength) >= 4 ? std::min(n, 3 + sectionLength) - 4 : 0;   // minus CRC
    if (pat) {
        if (p[0] != 0x00) return;
        for (size_t i = 8; i + 4 <= end; i += 4) {
            const int program = (p[i] << 8) | p[i + 1];
            const int pid = ((p[i + 2] & 0x1F) << 8) | p[i + 3];
            if (program != 0) {
                pmtPid_ = pid;
                break;
            }
        }
        return;
    }
    if (p[0] != 0x02 || end < 12) return;
    const size_t infoLength = ((p[10] & 0x0F) << 8) | p[11];
    bool unsupportedSeen = false;
    int firstAudio = -1, firstId3 = -1;
    Codec firstCodec = Codec::None;
    bool audioListed = false, id3Listed = false;
    for (size_t i = 12 + infoLength; i + 5 <= end;) {
        const uint8_t type = p[i];
        const int pid = ((p[i + 1] & 0x1F) << 8) | p[i + 2];
        const size_t esInfo = ((p[i + 3] & 0x0F) << 8) | p[i + 4];
        i += 5 + esInfo;
        if (type == 0x0F || type == 0x03 || type == 0x04) {
            audioListed = audioListed || pid == audioPid_;
            if (firstAudio < 0) {
                firstAudio = pid;
                firstCodec = type == 0x0F ? Codec::Aac : Codec::Mpeg;
            }
        } else if (type == 0x15) {
            id3Listed = id3Listed || pid == id3Pid_;
            if (firstId3 < 0) firstId3 = pid;
        } else if (type == 0x11 || type == 0x81 || type == 0x87 || type == 0xCF || type == 0xC1 || type == 0xC2 || type == 0x06) {
            unsupportedSeen = true;   // LATM AAC, AC-3, E-AC-3, SAMPLE-AES, private (Opus / AC-3 by descriptor)
        }
    }
    // A table cut off by the packet end lists only part of the streams: it may add one, never drop the current one.
    const bool complete = 3 + sectionLength <= n;
    if (!audioListed && (complete || (audioPid_ < 0 && firstAudio >= 0))) {
        audioPid_ = firstAudio;
        codec_ = firstCodec;
        audioStarted_ = false;   // from its next PES start
    }
    if (!id3Listed && (complete || id3Pid_ < 0)) {
        if (id3Pid_ != firstId3) id3_.clear();
        id3Pid_ = firstId3;
    }
    unsupported_ = audioPid_ < 0 && unsupportedSeen;
}

// ---- Fmp4Demuxer --------------------------------------------------------------------------------------------------

bool Fmp4Demuxer::init(const uint8_t* data, size_t size) {
    const auto moov = findBox(data, size, fourcc("moov"));
    if (!moov) return false;
    trackId_ = 0;
    forEachBox(moov->body, moov->size, [&](const Box& trak) {
        if (trak.type != fourcc("trak")) return true;
        const auto tkhd = findBox(trak.body, trak.size, fourcc("tkhd"));
        const auto mdia = findBox(trak.body, trak.size, fourcc("mdia"));
        if (!tkhd || !mdia || tkhd->size < 24) return true;
        const uint32_t id = be32(tkhd->body + (tkhd->body[0] == 1 ? 20 : 12));
        const auto hdlr = findBox(mdia->body, mdia->size, fourcc("hdlr"));
        if (!hdlr || hdlr->size < 12 || be32(hdlr->body + 8) != fourcc("soun")) return true;
        const auto minf = findBox(mdia->body, mdia->size, fourcc("minf"));
        const auto stbl = minf ? findBox(minf->body, minf->size, fourcc("stbl")) : std::nullopt;
        const auto stsd = stbl ? findBox(stbl->body, stbl->size, fourcc("stsd")) : std::nullopt;
        if (!stsd || stsd->size < 8) return true;
        std::optional<Box> entry;
        forEachBox(stsd->body + 8, stsd->size - 8, [&](const Box& b) {
            entry = b;
            return false;
        });
        if (!entry || entry->size < 28) return true;
        const uint8_t* e = entry->body;
        const uint32_t version = be16(e + 8);
        const size_t childStart = 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
        if (childStart > entry->size) return true;
        const uint32_t channels = be16(e + 16);
        const uint32_t rate = be32(e + 24) >> 16;
        if (entry->type == fourcc(".mp3")) {
            trackId_ = id;
            codec_ = Codec::Mpeg;
            return false;
        }
        if (entry->type != fourcc("mp4a")) return true;
        const auto esds = findBox(e + childStart, entry->size - childStart, fourcc("esds"));
        if (!esds || esds->size < 4) return true;
        const uint8_t* p = esds->body + 4;
        const uint8_t* end = esds->body + esds->size;
        uint8_t tag = 0;
        size_t len = 0;
        if (!descriptor(p, end, tag, len) || tag != 0x03 || len < 3) return true;
        const uint8_t flags = p[2];
        p += 3;
        if (flags & 0x80) p += 2;
        if (flags & 0x40) p += p < end ? 1 + size_t(*p) : 0;
        if (flags & 0x20) p += 2;
        if (p >= end || !descriptor(p, end, tag, len) || tag != 0x04 || len < 13) return true;
        const uint8_t oti = p[0];
        const uint8_t* dsi = p + 13;
        const uint8_t* dsiEnd = p + len;
        if (oti == 0x69 || oti == 0x6B) {
            trackId_ = id;
            codec_ = Codec::Mpeg;
            return false;
        }
        if (oti != 0x40 && oti != 0x66 && oti != 0x67 && oti != 0x68) return true;
        uint8_t aot = oti == 0x40 ? 0 : static_cast<uint8_t>(oti - 0x65);   // MPEG-2 AAC Main / LC / SSR
        uint8_t fi = freqIndexOf(rate), cc = static_cast<uint8_t>(std::min<uint32_t>(channels, 7));
        if (dsi < dsiEnd && descriptor(dsi, dsiEnd, tag, len) && tag == 0x05 && len >= 2) {
            BitReader br{dsi, len};
            uint32_t objectType = br.read(5);
            if (objectType == 31) objectType = 32 + br.read(6);
            uint32_t freq = br.read(4);
            if (freq == 15) freq = freqIndexOf(br.read(24));
            const uint32_t config = br.read(4);
            if (objectType == 5 || objectType == 29) {   // explicit SBR / PS: the core type follows
                if (br.read(4) == 15) br.read(24);
                objectType = br.read(5);
            }
            if (!br.ok()) return true;
            aot = static_cast<uint8_t>(objectType);
            fi = static_cast<uint8_t>(freq);
            cc = static_cast<uint8_t>(config);
        }
        if (aot < 1 || aot > 4 || fi >= 13) return true;   // ADTS can only carry object types 1..4
        trackId_ = id;
        codec_ = Codec::Aac;
        objectType_ = aot;
        freqIndex_ = fi;
        channelConfig_ = cc;
        return false;
    });
    if (!trackId_) return false;
    defaultSize_ = 0;
    if (const auto mvex = findBox(moov->body, moov->size, fourcc("mvex"))) {
        forEachBox(mvex->body, mvex->size, [&](const Box& b) {
            if (b.type == fourcc("trex") && b.size >= 24 && be32(b.body + 4) == trackId_) defaultSize_ = be32(b.body + 16);
            return true;
        });
    }
    return true;
}

bool Fmp4Demuxer::feed(const uint8_t* data, size_t size, std::vector<uint8_t>& es) {
    if (!trackId_) return false;
    bool any = false;
    std::optional<Box> moof;
    forEachBox(data, size, [&](const Box& box) {
        if (box.type == fourcc("moof")) {
            moof = box;
            return true;
        }
        if (box.type != fourcc("mdat") || !moof) return true;
        const size_t moofStart = moof->offset;
        const size_t mdatStart = static_cast<size_t>(box.body - data);
        forEachBox(moof->body, moof->size, [&](const Box& traf) {
            if (traf.type != fourcc("traf")) return true;
            const auto tfhd = findBox(traf.body, traf.size, fourcc("tfhd"));
            if (!tfhd || tfhd->size < 8 || be32(tfhd->body + 4) != trackId_) return true;
            const uint32_t tfFlags = be24(tfhd->body + 1);
            size_t q = 8;
            uint64_t base = moofStart;
            uint32_t defaultSize = defaultSize_;
            if (tfFlags & 0x01) {
                if (q + 8 > tfhd->size) return true;
                base = be64(tfhd->body + q);
                q += 8;
            }
            if (tfFlags & 0x02) q += 4;
            if (tfFlags & 0x08) q += 4;
            if (tfFlags & 0x10) {
                if (q + 4 > tfhd->size) return true;
                defaultSize = be32(tfhd->body + q);
            }
            uint64_t cursor = mdatStart;   // where the next run's data starts when it gives no data offset
            forEachBox(traf.body, traf.size, [&](const Box& trun) {
                if (trun.type != fourcc("trun") || trun.size < 8) return true;
                const uint32_t flags = be24(trun.body + 1);
                const uint32_t count = std::min<uint32_t>(be32(trun.body + 4), 20000);
                size_t r = 8;
                uint64_t pos = cursor;
                if (flags & 0x01) {
                    if (r + 4 > trun.size) return true;
                    pos = base + static_cast<int32_t>(be32(trun.body + r));
                    r += 4;
                }
                if (flags & 0x04) r += 4;
                const size_t perSample = ((flags & 0x100) ? 4 : 0) + ((flags & 0x200) ? 4 : 0) + ((flags & 0x400) ? 4 : 0) +
                                         ((flags & 0x800) ? 4 : 0);
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t sampleSize = defaultSize;
                    if (perSample) {
                        if (r + perSample > trun.size) break;
                        size_t f = r + ((flags & 0x100) ? 4 : 0);
                        if (flags & 0x200) sampleSize = be32(trun.body + f);
                        r += perSample;
                    }
                    if (sampleSize == 0 || pos > size || sampleSize > size - pos) break;
                    if (codec_ == Codec::Aac) {
                        if (sampleSize > 8191 - 7) break;
                        const auto header = makeAdtsHeader(objectType_, freqIndex_, channelConfig_, sampleSize);
                        es.insert(es.end(), header.begin(), header.end());
                    }
                    es.insert(es.end(), data + pos, data + pos + sampleSize);
                    pos += sampleSize;
                    any = true;
                }
                cursor = pos;
                return true;
            });
            return true;
        });
        moof.reset();
        return true;
    });
    return any;
}

} // namespace st::audio::live
