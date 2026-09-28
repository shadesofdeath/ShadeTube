#include "audio/ReplayGain.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace st::audio::replaygain {

namespace {

constexpr char kKey[] = "replaygain_track_gain";
constexpr size_t kMaxTagBytes = 16u << 20;    // a tag read in one piece (unsynchronised ID3, MP4 moov)
constexpr size_t kMaxFieldBytes = 1u << 16;   // one TXXX frame / Vorbis comment block / ilst item worth reading

// Random access over a file or a memory block.
class Reader {
public:
    virtual ~Reader() = default;
    virtual uint64_t size() const = 0;
    // Reads exactly n bytes at `offset`; false when they aren't all there.
    virtual bool read(uint64_t offset, void* dst, size_t n) = 0;
    bool read(uint64_t offset, std::vector<uint8_t>& dst, size_t n) {
        dst.resize(n);
        return read(offset, dst.data(), n);
    }
};

class MemoryReader final : public Reader {
public:
    MemoryReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint64_t size() const override { return size_; }
    bool read(uint64_t offset, void* dst, size_t n) override {
        if (offset > size_ || n > size_ - offset) return false;
        std::memcpy(dst, data_ + offset, n);
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
};

class FileReader final : public Reader {
public:
    explicit FileReader(const std::filesystem::path& path) {
        f_ = _wfopen(path.c_str(), L"rb");
        if (f_ && _fseeki64(f_, 0, SEEK_END) == 0) size_ = static_cast<uint64_t>(std::max<int64_t>(0, _ftelli64(f_)));
    }
    ~FileReader() override {
        if (f_) fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }
    uint64_t size() const override { return size_; }
    bool read(uint64_t offset, void* dst, size_t n) override {
        if (!f_ || offset > size_ || n > size_ - offset) return false;
        if (_fseeki64(f_, static_cast<int64_t>(offset), SEEK_SET) != 0) return false;
        return fread(dst, 1, n, f_) == n;
    }

private:
    FILE* f_ = nullptr;
    uint64_t size_ = 0;
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint32_t syncsafe(const uint8_t* p) { return uint32_t(p[0] & 0x7F) << 21 | uint32_t(p[1] & 0x7F) << 14 | uint32_t(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

bool equalsKey(std::string_view s) {
    if (s.size() != sizeof(kKey) - 1) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != kKey[i]) return false;
    return true;
}

// ID3 text in `enc` to plain ASCII-ish bytes (only digits, signs and "dB" matter): UTF-16 code units < 128 are kept.
std::string id3Text(const uint8_t* p, size_t n, uint8_t enc) {
    std::string out;
    if (enc == 1 || enc == 2) {
        bool bigEndian = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) {   // BOM
            if (p[0] == 0xFE && p[1] == 0xFF) bigEndian = true;
            i = 2;
        }
        for (; i + 1 < n; i += 2) {
            const unsigned cu = bigEndian ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (cu == 0) break;
            out.push_back(cu < 128 ? static_cast<char>(cu) : '?');
        }
    } else {
        for (size_t i = 0; i < n && p[i]; ++i) out.push_back(static_cast<char>(p[i]));
    }
    return out;
}

// TXXX body: encoding, description (terminated), value.
std::optional<float> txxx(const uint8_t* p, size_t n) {
    if (n < 2) return std::nullopt;
    const uint8_t enc = p[0];
    const bool wide = enc == 1 || enc == 2;
    size_t i = 1;
    // Find the description terminator (one zero byte, or an aligned zero code unit for UTF-16).
    size_t end = i;
    if (wide) {
        while (end + 1 < n && !(p[end] == 0 && p[end + 1] == 0)) end += 2;
    } else {
        while (end < n && p[end] != 0) ++end;
    }
    if (end >= n) return std::nullopt;
    const std::string desc = id3Text(p + i, end - i, enc);
    if (!equalsKey(desc)) return std::nullopt;
    const size_t value = end + (wide ? 2 : 1);
    if (value > n) return std::nullopt;
    return parseGain(id3Text(p + value, n - value, enc));
}

std::optional<float> id3(Reader& r) {
    uint8_t h[10];
    if (!r.read(0, h, 10) || std::memcmp(h, "ID3", 3) != 0) return std::nullopt;
    const uint8_t major = h[3], flags = h[5];
    if (major < 2 || major > 4) return std::nullopt;
    const uint64_t tagEnd = 10 + uint64_t(syncsafe(h + 6));
    // A tag-wide unsynchronisation (v2.2 / v2.3) scrambles frame boundaries: read the tag in one piece and undo it.
    if ((flags & 0x80) && major < 4) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(tagEnd - 10, kMaxTagBytes));
        std::vector<uint8_t> raw;
        if (!r.read(10, raw, std::min<uint64_t>(n, r.size() > 10 ? r.size() - 10 : 0))) return std::nullopt;
        std::vector<uint8_t> tag;
        tag.reserve(raw.size() + 10);
        tag.insert(tag.end(), h, h + 10);
        tag[5] &= 0x7F;
        for (size_t i = 0; i < raw.size(); ++i) {
            tag.push_back(raw[i]);
            if (raw[i] == 0xFF && i + 1 < raw.size() && raw[i + 1] == 0x00) ++i;
        }
        const size_t len = tag.size() - 10;   // the tag shrank: fix its (syncsafe) size
        for (int k = 0; k < 4; ++k) tag[6 + k] = static_cast<uint8_t>((len >> (7 * (3 - k))) & 0x7F);
        MemoryReader m(tag.data(), tag.size());
        return id3(m);
    }
    uint64_t pos = 10;
    if ((flags & 0x40) && major >= 3) {   // extended header
        uint8_t e[4];
        if (!r.read(pos, e, 4)) return std::nullopt;
        pos += major == 4 ? syncsafe(e) : 4 + uint64_t(be32(e));
    }
    const size_t headerLen = major == 2 ? 6 : 10;
    std::vector<uint8_t> body;
    while (pos + headerLen <= tagEnd) {
        uint8_t fh[10];
        if (!r.read(pos, fh, headerLen)) break;
        if (fh[0] == 0) break;   // padding
        const uint64_t size = major == 2 ? be24(fh + 3) : major == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        const uint64_t data = pos + headerLen;
        if (size == 0 || data + size > tagEnd) break;
        const bool isTxxx = major == 2 ? std::memcmp(fh, "TXX", 3) == 0 : std::memcmp(fh, "TXXX", 4) == 0;
        // v2.4 per-frame flags: compression / encryption can't be read here; unsynchronisation is undone below.
        const uint8_t format = major == 4 ? fh[9] : 0;
        if (isTxxx && size <= kMaxFieldBytes && !(format & 0x0C)) {
            if (!r.read(data, body, static_cast<size_t>(size))) break;
            size_t skip = format & 0x01 ? 4 : 0;   // data length indicator
            if (format & 0x02) {                   // frame unsynchronisation
                std::vector<uint8_t> clean;
                for (size_t i = 0; i < body.size(); ++i) {
                    clean.push_back(body[i]);
                    if (body[i] == 0xFF && i + 1 < body.size() && body[i + 1] == 0x00) ++i;
                }
                body.swap(clean);
            }
            if (skip < body.size())
                if (auto g = txxx(body.data() + skip, body.size() - skip)) return g;
        }
        pos = data + size;
    }
    return std::nullopt;
}

uint64_t id3Size(Reader& r) {
    uint8_t h[10];
    if (!r.read(0, h, 10) || std::memcmp(h, "ID3", 3) != 0) return 0;
    return 10 + uint64_t(syncsafe(h + 6)) + ((h[5] & 0x10) ? 10 : 0);   // + footer
}

std::optional<float> flac(Reader& r) {
    uint64_t pos = id3Size(r);   // a (non-standard) ID3 tag in front
    uint8_t magic[4];
    if (!r.read(pos, magic, 4) || std::memcmp(magic, "fLaC", 4) != 0) return std::nullopt;
    pos += 4;
    std::vector<uint8_t> block;
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t h[4];
        if (!r.read(pos, h, 4)) return std::nullopt;
        const bool last = h[0] & 0x80;
        const uint8_t type = h[0] & 0x7F;
        const uint32_t len = be24(h + 1);
        if (type == 4 && len <= kMaxTagBytes) {   // VORBIS_COMMENT (little-endian lengths)
            if (!r.read(pos + 4, block, len)) return std::nullopt;
            const uint8_t* p = block.data();
            const size_t n = block.size();
            if (n < 8) return std::nullopt;
            size_t i = 4 + size_t(le32(p));   // vendor string
            if (i + 4 > n) return std::nullopt;
            const uint32_t count = le32(p + i);
            i += 4;
            for (uint32_t k = 0; k < count && i + 4 <= n; ++k) {
                const uint32_t cl = le32(p + i);
                i += 4;
                if (cl > n - i) break;
                const std::string_view comment(reinterpret_cast<const char*>(p + i), cl);
                i += cl;
                const size_t eq = comment.find('=');
                if (eq != std::string_view::npos && equalsKey(comment.substr(0, eq))) return parseGain(comment.substr(eq + 1));
            }
            return std::nullopt;
        }
        if (last) break;
        pos += 4 + uint64_t(len);
    }
    return std::nullopt;
}

// ---- MP4 ----------------------------------------------------------------------------------------------------------
struct Box {
    uint64_t start = 0, content = 0, end = 0;   // absolute offsets
    char type[5] = {};
};

// Box header at `pos` inside [.., limit): false when there is none.
bool boxAt(Reader& r, uint64_t pos, uint64_t limit, Box& b) {
    uint8_t h[16];
    if (pos + 8 > limit || !r.read(pos, h, 8)) return false;
    uint64_t size = be32(h);
    uint64_t header = 8;
    if (size == 1) {
        if (pos + 16 > limit || !r.read(pos + 8, h + 8, 8)) return false;
        size = uint64_t(be32(h + 8)) << 32 | be32(h + 12);
        header = 16;
    } else if (size == 0) {
        size = limit - pos;
    }
    if (size < header || pos + size > limit) return false;
    b.start = pos;
    b.content = pos + header;
    b.end = pos + size;
    std::memcpy(b.type, h + 4, 4);
    return true;
}

bool findChild(Reader& r, uint64_t from, uint64_t to, const char* type, Box& out) {
    Box b;
    for (uint64_t pos = from; boxAt(r, pos, to, b); pos = b.end) {
        if (std::memcmp(b.type, type, 4) == 0) {
            out = b;
            return true;
        }
    }
    return false;
}

std::optional<float> mp4(Reader& r) {
    Box ftyp;
    if (!boxAt(r, 0, r.size(), ftyp) || std::memcmp(ftyp.type, "ftyp", 4) != 0) return std::nullopt;
    Box moov, udta, meta, ilst;
    if (!findChild(r, 0, r.size(), "moov", moov) || !findChild(r, moov.content, moov.end, "udta", udta) ||
        !findChild(r, udta.content, udta.end, "meta", meta))
        return std::nullopt;
    // "meta" is a full box (4 bytes version / flags) in iTunes files; QuickTime-style files omit them.
    uint64_t metaContent = meta.content + 4;
    if (Box probe; boxAt(r, meta.content, meta.end, probe) && std::memcmp(probe.type, "hdlr", 4) == 0) metaContent = meta.content;
    if (!findChild(r, metaContent, meta.end, "ilst", ilst)) return std::nullopt;
    Box item;
    std::vector<uint8_t> buf;
    for (uint64_t pos = ilst.content; boxAt(r, pos, ilst.end, item); pos = item.end) {
        if (std::memcmp(item.type, "----", 4) != 0) continue;
        std::string name;
        std::optional<float> value;
        Box sub;
        for (uint64_t sp = item.content; boxAt(r, sp, item.end, sub); sp = sub.end) {
            const uint64_t len = sub.end - sub.content;
            if (len < 4 || len > kMaxFieldBytes) continue;
            if (std::memcmp(sub.type, "name", 4) == 0 && r.read(sub.content, buf, static_cast<size_t>(len))) {
                name.assign(reinterpret_cast<const char*>(buf.data()) + 4, buf.size() - 4);   // version / flags
            } else if (std::memcmp(sub.type, "data", 4) == 0 && len > 8 && r.read(sub.content, buf, static_cast<size_t>(len))) {
                value = parseGain(std::string_view(reinterpret_cast<const char*>(buf.data()) + 8, buf.size() - 8));
            }
        }
        if (equalsKey(name) && value) return value;
    }
    return std::nullopt;
}

std::optional<float> lookup(Reader& r) {
    if (auto g = id3(r)) return g;
    if (auto g = flac(r)) return g;
    return mp4(r);
}

} // namespace

std::optional<float> parseGain(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    std::string num;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) num.push_back(s[i++]);
    bool digits = false, point = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            num.push_back(c);
            digits = true;
        } else if ((c == '.' || c == ',') && !point) {
            num.push_back('.');
            point = true;
        } else {
            break;
        }
    }
    if (!digits) return std::nullopt;
    while (i < s.size() && s[i] == ' ') ++i;
    const std::string_view rest = s.substr(i);
    if (!rest.empty()) {
        const bool db = rest.size() >= 2 && (rest[0] == 'd' || rest[0] == 'D') && (rest[1] == 'b' || rest[1] == 'B');
        if (!db) return std::nullopt;
        for (char c : rest.substr(2))
            if (c != ' ' && c != '\0') return std::nullopt;
    }
    const float v = std::strtof(num.c_str(), nullptr);
    if (!(v >= -64.f && v <= 64.f)) return std::nullopt;
    return v;
}

std::optional<float> fromId3(const uint8_t* data, size_t size) {
    MemoryReader r(data, size);
    return id3(r);
}
std::optional<float> fromFlac(const uint8_t* data, size_t size) {
    MemoryReader r(data, size);
    return flac(r);
}
std::optional<float> fromMp4(const uint8_t* data, size_t size) {
    MemoryReader r(data, size);
    return mp4(r);
}

std::optional<float> readTrackGainDb(const std::filesystem::path& file) {
    FileReader r(file);
    if (!r.ok()) return std::nullopt;
    return lookup(r);
}

} // namespace st::audio::replaygain
