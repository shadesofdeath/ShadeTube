#include "audio/TagWriter.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <tuple>
#include <variant>

namespace st::audio::tags {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr size_t kMaxTagBytes = 64u << 20;      // an ID3 tag / the FLAC metadata / moov read into memory
constexpr size_t kMaxMoovBytes = 128u << 20;
constexpr uint32_t kFlacPadding = 4096;         // padding of rewritten FLAC metadata
constexpr uint32_t kId3Padding = 2048;          // padding of a rewritten (grown) ID3 tag

// ---- errors (converted to WriteResult / ReadResult at the API) ---------------------------------------------------
struct Fail {
    Status status;
    std::string detail;
};
[[noreturn]] void fail(Status s, std::string detail) { throw Fail{s, std::move(detail)}; }

// ---- bytes ------------------------------------------------------------------------------------------------------
uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint32_t syncsafe(const uint8_t* p) {
    return uint32_t(p[0] & 0x7F) << 21 | uint32_t(p[1] & 0x7F) << 14 | uint32_t(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

void putBE16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void putBE24(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 16));
    putBE16(b, v);
}
void putBE32(Bytes& b, uint32_t v) {
    putBE16(b, v >> 16);
    putBE16(b, v & 0xFFFF);
}
void putBE64(Bytes& b, uint64_t v) {
    putBE32(b, static_cast<uint32_t>(v >> 32));
    putBE32(b, static_cast<uint32_t>(v));
}
void putLE32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putSyncsafe(Bytes& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>((v >> (7 * i)) & 0x7F));
}
void append(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
void append(Bytes& b, const Bytes& s) { b.insert(b.end(), s.begin(), s.end()); }
void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
void writeBE64(uint8_t* p, uint64_t v) {
    writeBE32(p, static_cast<uint32_t>(v >> 32));
    writeBE32(p + 4, static_cast<uint32_t>(v));
}

// ---- text -------------------------------------------------------------------------------------------------------
std::string trim(std::string_view s) {
    const auto sp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    size_t a = 0, b = s.size();
    while (a < b && sp(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && sp(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::string latin1ToUtf8(const uint8_t* p, size_t n) {
    std::wstring w;
    for (size_t i = 0; i < n && p[i]; ++i) w.push_back(static_cast<wchar_t>(p[i]));
    return toUtf8(w);
}

std::string utf16ToUtf8(const uint8_t* p, size_t n, bool bigEndian) {
    std::wstring w;
    for (size_t i = 0; i + 1 < n; i += 2) {
        const wchar_t c = static_cast<wchar_t>(bigEndian ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]));
        if (c == 0) break;
        w.push_back(c);
    }
    return toUtf8(w);
}

// Best-fit Latin-1 (ID3v1 / ID3 ISO-8859-1 frames): "ş" -> "s", what has no fit -> "?".
std::string toLatin1(const std::string& utf8) {
    const std::wstring w = toWide(utf8);
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(28591, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(28591, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

int leadingInt(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    int v = 0, digits = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 9; ++i, ++digits) v = v * 10 + (s[i] - '0');
    return digits ? v : 0;
}

// "3/12" -> {3, 12}
std::pair<int, int> numberPair(std::string_view s) {
    const int a = leadingInt(s);
    const size_t slash = s.find('/');
    return {a, slash == std::string_view::npos ? 0 : leadingInt(s.substr(slash + 1))};
}

int yearOf(std::string_view s) {
    const std::string t = trim(s);
    if (t.size() < 4) return 0;
    for (int i = 0; i < 4; ++i)
        if (t[i] < '0' || t[i] > '9') return 0;
    return leadingInt(std::string_view(t).substr(0, 4));
}

// The year written into an existing date ("2019-05-03" + 2021 -> "2021-05-03"); a bare year otherwise.
std::string withYear(const std::string& existing, int year) {
    char y[8];
    std::snprintf(y, sizeof y, "%04d", year);
    const std::string t = trim(existing);
    if (yearOf(t) > 0 && t.size() > 4 && (t[4] == '-' || t[4] == 'T')) return std::string(y) + t.substr(4);
    return y;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string upperAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string mimeOf(const Bytes& d) {
    if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (d.size() >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "image/png";
    if (d.size() >= 3 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return "image/gif";
    if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
    return {};
}

// ---- files ------------------------------------------------------------------------------------------------------
std::wstring winPath(const fs::path& p) {
    std::wstring s = p.wstring();
    if (s.size() < 240 || s.starts_with(L"\\\\?\\")) return s;
    if (s.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + s.substr(2);
    return L"\\\\?\\" + s;
}

uint64_t fileTime(const FILETIME& ft) { return uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime; }

Status statusForError(DWORD err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return Status::NotFound;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE: return Status::InUse;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT: return Status::ReadOnly;
    default: return Status::IoError;
    }
}

// Random access reads of one file (shares everything: it is the user's file, other programs may have it open).
class Source {
public:
    explicit Source(const fs::path& path) {
        h_ = CreateFileW(winPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            error_ = GetLastError();
            return;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h_, &info)) {
            error_ = GetLastError();
            close();
            return;
        }
        size_ = uint64_t(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
        mtime_ = fileTime(info.ftLastWriteTime);
        attributes_ = info.dwFileAttributes;
    }
    ~Source() { close(); }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    DWORD error() const { return error_; }
    uint64_t size() const { return size_; }
    uint64_t mtime() const { return mtime_; }
    DWORD attributes() const { return attributes_; }
    void close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

    bool read(uint64_t offset, void* dst, size_t n) {
        if (!ok() || offset > size_ || n > size_ - offset) return false;
        auto* out = static_cast<uint8_t*>(dst);
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD want = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
            DWORD got = 0;
            if (!ReadFile(h_, out, want, &got, &ov) || got == 0) return false;
            out += got;
            offset += got;
            n -= got;
        }
        return true;
    }
    Bytes read(uint64_t offset, size_t n) {
        Bytes b(n);
        if (n > 0 && !read(offset, b.data(), n)) fail(Status::Corrupt, "truncated file");
        return b;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    DWORD error_ = 0;
    uint64_t size_ = 0, mtime_ = 0;
    DWORD attributes_ = 0;
};

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
            BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0)
            fail(Status::IoError, "SHA-256 unavailable");
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const uint8_t* p, size_t n) {
        if (n > 0) BCryptHashData(hash_, const_cast<PUCHAR>(p), static_cast<ULONG>(n), 0);
    }
    std::string hex() {
        uint8_t d[32]{};
        BCryptFinishHash(hash_, d, sizeof d, 0);
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (uint8_t b : d) {
            s += digits[b >> 4];
            s += digits[b & 15];
        }
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;   // [from, to) in file order

std::string hashRanges(Source& s, const Ranges& ranges) {
    Sha256 h;
    Bytes buf(1u << 20);
    for (const auto& [from, to] : ranges) {
        for (uint64_t pos = from; pos < to;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), to - pos));
            if (!s.read(pos, buf.data(), n)) fail(Status::Corrupt, "truncated payload");
            h.add(buf.data(), n);
            pos += n;
        }
    }
    return h.hex();
}

// ---- the new file: literal bytes and copied ranges of the original ----------------------------------------------
struct Patch {
    uint64_t at;   // absolute offset in the original
    Bytes bytes;
};
struct Piece {
    Bytes bytes;   // literal (copy = false)
    bool copy = false;
    uint64_t from = 0, to = 0;   // the original's range (copy)
    std::vector<Patch> patches;  // within [from, to)
};
using Plan = std::vector<Piece>;
Piece literal(Bytes b) {
    Piece p;
    p.bytes = std::move(b);
    return p;
}
Piece copyRange(uint64_t from, uint64_t to, std::vector<Patch> patches = {}) {
    Piece p;
    p.copy = true;
    p.from = from;
    p.to = to;
    p.patches = std::move(patches);
    return p;
}

// ---- ID3v1 genres -------------------------------------------------------------------------------------------------
constexpr const char* kGenres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult",
    "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop",
    "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
    "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson",
    "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam",
    "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
    "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal",
    "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
    "Synthpop"};

// ---- formats -------------------------------------------------------------------------------------------------------
bool isFree(const char* type) { return std::memcmp(type, "free", 4) == 0 || std::memcmp(type, "skip", 4) == 0; }

std::string extensionOf(const fs::path& file) { return lowerAscii(toUtf8(file.extension().wstring())); }

// What the edit touches (text fields), in the order new fields are appended.
enum Field { kTitle, kArtist, kAlbumArtist, kAlbum, kTrack, kDisc, kYear, kGenre, kFieldCount };

bool edits(const Edit& e, Field f) {
    switch (f) {
    case kTitle: return e.title.has_value();
    case kArtist: return e.artist.has_value();
    case kAlbumArtist: return e.albumArtist.has_value();
    case kAlbum: return e.album.has_value();
    case kTrack: return e.track.has_value();
    case kDisc: return e.disc.has_value();
    case kYear: return e.year.has_value();
    case kGenre: return e.genre.has_value();
    default: return false;
    }
}

// =====================================================================================================================
// ID3v2 + ID3v1 (MP3)
namespace id3 {

struct Frame {
    std::string id;           // 4 characters (v2.2 ids converted)
    uint8_t flags[2] = {0, 0};
    Bytes body;               // as stored (v2.4: may be unsynchronised / carry a data length indicator: see flags)
    bool operator==(const Frame& o) const {
        return id == o.id && flags[0] == o.flags[0] && flags[1] == o.flags[1] && body == o.body;
    }
};

struct Parsed {
    bool hasTag = false;
    int major = 3;            // the version written back (a v2.2 tag becomes v2.3)
    uint64_t tagEnd = 0;      // where the audio starts (0 = no tag)
    std::vector<Frame> frames;
    bool junk = false;        // something that is neither a frame nor zero padding: not rewritten
    std::string unsupported;  // a tag this code can't rewrite (v2.2 compression)
    bool hasV1 = false;
    std::array<uint8_t, 128> v1{};
    uint64_t audioEnd = 0;    // the file size, or where the ID3v1 tag starts
};

bool validId(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return false;
    return true;
}

Bytes deunsync(const Bytes& in) {
    Bytes out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        out.push_back(in[i]);
        if (in[i] == 0xFF && i + 1 < in.size() && in[i + 1] == 0x00) ++i;
    }
    return out;
}

// v2.4 frame sizes are syncsafe, but some writers (old iTunes) stored plain big-endian ones: take the reading that
// lands on the next frame (or the padding / the end).
uint32_t v4Size(const Bytes& t, size_t pos) {
    const uint8_t* q = &t[pos + 4];
    const uint32_t ss = syncsafe(q), be = be32(q);
    if ((q[0] | q[1] | q[2] | q[3]) & 0x80) return be;
    if (ss == be) return ss;
    const auto lands = [&](uint64_t size) {
        const uint64_t next = pos + 10 + size;
        if (next > t.size()) return false;
        if (next + 10 > t.size()) return true;
        return t[next] == 0 || validId(&t[next], 4);
    };
    if (lands(ss)) return ss;
    if (lands(be)) return be;
    return ss;
}

// v2.2 -> v2.3 frame ids whose bodies are laid out the same way.
const char* v22to23(std::string_view id) {
    static const std::pair<const char*, const char*> table[] = {
        {"TT1", "TIT1"}, {"TT2", "TIT2"}, {"TT3", "TIT3"}, {"TP1", "TPE1"}, {"TP2", "TPE2"}, {"TP3", "TPE3"},
        {"TP4", "TPE4"}, {"TCM", "TCOM"}, {"TXT", "TEXT"}, {"TLA", "TLAN"}, {"TCO", "TCON"}, {"TAL", "TALB"},
        {"TPA", "TPOS"}, {"TRK", "TRCK"}, {"TRC", "TSRC"}, {"TYE", "TYER"}, {"TDA", "TDAT"}, {"TIM", "TIME"},
        {"TRD", "TRDA"}, {"TMT", "TMED"}, {"TFT", "TFLT"}, {"TBP", "TBPM"}, {"TCR", "TCOP"}, {"TPB", "TPUB"},
        {"TEN", "TENC"}, {"TSS", "TSSE"}, {"TOF", "TOFN"}, {"TLE", "TLEN"}, {"TSI", "TSIZ"}, {"TDY", "TDLY"},
        {"TKE", "TKEY"}, {"TOT", "TOAL"}, {"TOA", "TOPE"}, {"TOL", "TOLY"}, {"TOR", "TORY"}, {"TXX", "TXXX"},
        {"WAF", "WOAF"}, {"WAR", "WOAR"}, {"WAS", "WOAS"}, {"WCM", "WCOM"}, {"WCP", "WCOP"}, {"WPB", "WPUB"},
        {"WXX", "WXXX"}, {"IPL", "IPLS"}, {"MCI", "MCDI"}, {"ETC", "ETCO"}, {"MLL", "MLLT"}, {"STC", "SYTC"},
        {"ULT", "USLT"}, {"SLT", "SYLT"}, {"COM", "COMM"}, {"RVA", "RVAD"}, {"EQU", "EQUA"}, {"REV", "RVRB"},
        {"GEO", "GEOB"}, {"CNT", "PCNT"}, {"POP", "POPM"}, {"BUF", "RBUF"}, {"CRA", "AENC"}, {"UFI", "UFID"}};
    for (const auto& [from, to] : table)
        if (id == from) return to;
    return nullptr;
}

// v2.2 PIC (3-letter image format) -> v2.3 APIC (mime type).
Bytes picToApic(const Bytes& b) {
    if (b.size() < 5) return {};
    Bytes out{b[0]};
    const std::string fmt = upperAscii(std::string(b.begin() + 1, b.begin() + 4));
    append(out, fmt == "PNG" ? std::string_view("image/png") : fmt == "JPG" ? std::string_view("image/jpeg") : std::string_view("image/"));
    out.push_back(0);
    out.insert(out.end(), b.begin() + 4, b.end());
    return out;
}

Bytes textBody(const std::string& utf8);
std::string v1Field(const std::array<uint8_t, 128>& v1, size_t at, size_t len);

// A file with only an ID3v1 tag: the ID3v2 tag it gets starts with what the ID3v1 tag says (as if it held these
// frames already, so they count as kept, not as new).
void seedFromV1(Parsed& p) {
    if (!p.hasV1) return;
    const auto seed = [&](const char* id, const std::string& v) {
        if (!v.empty()) p.frames.push_back({id, {0, 0}, textBody(v)});
    };
    seed("TIT2", v1Field(p.v1, 3, 30));
    seed("TPE1", v1Field(p.v1, 33, 30));
    seed("TALB", v1Field(p.v1, 63, 30));
    const int year = yearOf(v1Field(p.v1, 93, 4));
    seed("TYER", year > 0 ? std::to_string(year) : std::string());
    seed("TRCK", p.v1[125] == 0 && p.v1[126] != 0 ? std::to_string(p.v1[126]) : std::string());
    seed("TCON", id3v1Genre(p.v1[127]));
}

Parsed parse(Source& s) {
    Parsed p;
    p.audioEnd = s.size();
    if (s.size() >= 128) {
        s.read(s.size() - 128, p.v1.data(), 128);
        if (p.v1[0] == 'T' && p.v1[1] == 'A' && p.v1[2] == 'G') {
            p.hasV1 = true;
            p.audioEnd = s.size() - 128;
        }
    }
    uint8_t h[10];
    if (s.size() < 10 || !s.read(0, h, 10) || std::memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4 || h[4] == 0xFF ||
        ((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        seedFromV1(p);
        return p;
    }
    p.hasTag = true;
    const int ver = h[3];
    const uint8_t flags = h[5];
    p.major = ver == 2 ? 3 : ver;
    const uint32_t size = syncsafe(h + 6);
    p.tagEnd = 10 + uint64_t(size) + ((ver == 4 && (flags & 0x10)) ? 10 : 0);
    if (p.tagEnd > p.audioEnd) fail(Status::Corrupt, "ID3 tag runs past the end of the file");
    if (size > kMaxTagBytes) fail(Status::TooLarge, "ID3 tag too large");
    if (ver == 2 && (flags & 0x40)) {
        p.unsupported = "compressed ID3v2.2 tag";
        return p;
    }
    Bytes tag = s.read(10, size);
    if (ver < 4 && (flags & 0x80)) tag = deunsync(tag);
    const bool allUnsync = ver == 4 && (flags & 0x80);
    size_t pos = 0;
    if (ver >= 3 && (flags & 0x40) && tag.size() >= 4) {   // extended header (dropped when written back)
        const uint64_t ext = ver == 4 ? syncsafe(tag.data()) : 4 + uint64_t(be32(tag.data()));
        if (ext > tag.size()) fail(Status::Corrupt, "bad ID3 extended header");
        pos = static_cast<size_t>(ext);
    }
    const size_t head = ver == 2 ? 6 : 10, idLen = ver == 2 ? 3 : 4;
    while (pos + head <= tag.size()) {
        if (tag[pos] == 0) {   // padding: must be zeros to the end
            p.junk = std::any_of(tag.begin() + static_cast<std::ptrdiff_t>(pos), tag.end(), [](uint8_t b) { return b != 0; });
            pos = tag.size();
            break;
        }
        if (!validId(&tag[pos], idLen)) {
            p.junk = true;
            break;
        }
        const uint32_t frameSize = ver == 2 ? be24(&tag[pos + 3]) : ver == 3 ? be32(&tag[pos + 4]) : v4Size(tag, pos);
        if (frameSize > tag.size() - pos - head) {
            p.junk = true;
            break;
        }
        const std::string id(tag.begin() + static_cast<std::ptrdiff_t>(pos), tag.begin() + static_cast<std::ptrdiff_t>(pos + idLen));
        const uint8_t flag0 = ver == 2 ? 0 : tag[pos + 8], flag1 = ver == 2 ? 0 : tag[pos + 9];
        Bytes body(tag.begin() + static_cast<std::ptrdiff_t>(pos + head),
                   tag.begin() + static_cast<std::ptrdiff_t>(pos + head + frameSize));
        pos += head + frameSize;
        Frame f;
        if (ver == 2) {
            if (id == "PIC") {
                f.id = "APIC";
                f.body = picToApic(body);
                if (f.body.empty()) continue;
            } else if (const char* to = v22to23(id)) {
                f.id = to;
                f.body = std::move(body);
            } else {
                ST_LOG_INFO("tags", "ID3v2.2 frame {} has no v2.3 form: dropped", id);
                continue;
            }
        } else {
            f.id = id;
            f.flags[0] = flag0;
            f.flags[1] = flag1;
            if (allUnsync) f.flags[1] |= 0x02;   // the header said so for every frame
            f.body = std::move(body);
        }
        p.frames.push_back(std::move(f));
    }
    if (pos < tag.size() && !p.junk)
        p.junk = std::any_of(tag.begin() + static_cast<std::ptrdiff_t>(pos), tag.end(), [](uint8_t b) { return b != 0; });
    return p;
}

// A frame's content (flags undone), "" when it can't be read (compressed / encrypted).
Bytes content(const Frame& f, int major) {
    Bytes b = f.body;
    if (major == 3) {
        if (f.flags[1] & 0xC0) return {};
        if ((f.flags[1] & 0x20) && !b.empty()) b.erase(b.begin());   // grouping id
        return b;
    }
    if (f.flags[1] & 0x0C) return {};
    if (f.flags[1] & 0x02) b = deunsync(b);
    if ((f.flags[1] & 0x40) && !b.empty()) b.erase(b.begin());      // grouping id
    if (f.flags[1] & 0x01) {                                        // data length indicator
        if (b.size() < 4) return {};
        b.erase(b.begin(), b.begin() + 4);
    }
    return b;
}

// Text frame values (v2.4 may hold several, NUL-separated).
std::vector<std::string> textValues(const Bytes& c) {
    std::vector<std::string> out;
    if (c.empty()) return out;
    const uint8_t enc = c[0];
    const uint8_t* p = c.data() + 1;
    const size_t n = c.size() - 1;
    const bool wide = enc == 1 || enc == 2;
    size_t i = 0;
    while (i < n) {
        size_t end = i;
        if (wide) {
            while (end + 1 < n && !(p[end] == 0 && p[end + 1] == 0)) end += 2;
            if (end + 1 >= n) end = n;
        } else {
            while (end < n && p[end] != 0) ++end;
        }
        std::string v;
        if (enc == 0) {
            v = latin1ToUtf8(p + i, end - i);
        } else if (enc == 3) {
            v.assign(reinterpret_cast<const char*>(p + i), end - i);
        } else {
            bool bigEndian = enc == 2;
            size_t k = i;
            if (enc == 1 && end - k >= 2) {
                if (p[k] == 0xFE && p[k + 1] == 0xFF) {
                    bigEndian = true;
                    k += 2;
                } else if (p[k] == 0xFF && p[k + 1] == 0xFE) {
                    k += 2;
                }
            }
            v = utf16ToUtf8(p + k, end - k, bigEndian);
        }
        out.push_back(trim(v));
        i = end + (wide ? 2 : 1);
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        if (x.empty()) continue;
        if (!s.empty()) s += "; ";
        s += x;
    }
    return s;
}

// TCON: "(17)", "(17)Rock", "17" (v2.4), "(RX)" / "(CR)" -> names.
std::string genreName(std::string v) {
    v = trim(v);
    if (v.size() >= 3 && v[0] == '(' && v[1] != '(') {
        const size_t close = v.find(')');
        if (close != std::string::npos) {
            const std::string ref = v.substr(1, close - 1), rest = trim(v.substr(close + 1));
            if (!rest.empty()) return rest;
            if (ref == "RX") return "Remix";
            if (ref == "CR") return "Cover";
            if (!ref.empty() && std::all_of(ref.begin(), ref.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return id3v1Genre(leadingInt(ref));
        }
    }
    if (v.starts_with("((")) return v.substr(1);
    if (!v.empty() && v.size() <= 3 && std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const std::string name = id3v1Genre(leadingInt(v));
        if (!name.empty()) return name;
    }
    return v;
}

std::optional<Picture> apic(const Bytes& c) {
    if (c.size() < 4) return std::nullopt;
    const uint8_t enc = c[0];
    size_t p = 1;
    while (p < c.size() && c[p] != 0) ++p;
    std::string mime = lowerAscii(std::string(c.begin() + 1, c.begin() + static_cast<std::ptrdiff_t>(p)));
    ++p;
    if (p >= c.size()) return std::nullopt;
    const int type = c[p++];
    if (enc == 1 || enc == 2) {
        while (p + 1 < c.size() && !(c[p] == 0 && c[p + 1] == 0)) p += 2;
        p += 2;
    } else {
        while (p < c.size() && c[p] != 0) ++p;
        ++p;
    }
    if (p > c.size()) return std::nullopt;
    Picture pic;
    pic.data.assign(c.begin() + static_cast<std::ptrdiff_t>(p), c.end());
    pic.type = type;
    const std::string sniffed = mimeOf(pic.data);
    pic.mime = !sniffed.empty() ? sniffed : mime;
    if (pic.data.empty()) return std::nullopt;
    return pic;
}

// The picture shown as the cover: the first front cover, else the first picture. Index into frames, -1 = none.
int coverFrame(const std::vector<Frame>& frames, int major) {
    int first = -1;
    for (int i = 0; i < static_cast<int>(frames.size()); ++i) {
        if (frames[i].id != "APIC") continue;
        const auto pic = apic(content(frames[i], major));
        if (!pic) continue;
        if (pic->type == 3) return i;
        if (first < 0) first = i;
    }
    return first;
}

std::string v1Field(const std::array<uint8_t, 128>& v1, size_t at, size_t len) { return trim(latin1ToUtf8(&v1[at], len)); }

Tags tagsOf(const Parsed& p) {
    Tags t;
    bool seen[kFieldCount] = {};
    const auto first = [&](Field f) {
        if (seen[f]) return false;
        seen[f] = true;
        return true;
    };
    std::string tyer, tdrc;
    for (const auto& f : p.frames) {
        if (f.id.size() != 4 || (f.id[0] != 'T' && f.id != "APIC")) continue;
        const Bytes c = content(f, p.major);
        if (c.empty()) continue;
        const auto v = textValues(c);
        if (f.id == "TIT2" && first(kTitle)) t.title = joined(v);
        else if (f.id == "TPE1" && first(kArtist)) t.artist = joined(v);
        else if (f.id == "TALB" && first(kAlbum)) t.album = joined(v);
        else if (f.id == "TPE2" && first(kAlbumArtist)) t.albumArtist = joined(v);
        else if (f.id == "TCON" && first(kGenre)) {
            std::vector<std::string> names;
            for (const auto& x : v) names.push_back(genreName(x));
            t.genre = joined(names);
        } else if (f.id == "TRCK" && first(kTrack) && !v.empty()) std::tie(t.track, t.trackTotal) = numberPair(v[0]);
        else if (f.id == "TPOS" && first(kDisc) && !v.empty()) std::tie(t.disc, t.discTotal) = numberPair(v[0]);
        else if (f.id == "TYER" && tyer.empty() && !v.empty()) tyer = v[0];
        else if (f.id == "TDRC" && tdrc.empty() && !v.empty()) tdrc = v[0];
    }
    t.year = p.major == 4 ? (yearOf(tdrc) ? yearOf(tdrc) : yearOf(tyer)) : (yearOf(tyer) ? yearOf(tyer) : yearOf(tdrc));
    if (const int i = coverFrame(p.frames, p.major); i >= 0) t.cover = apic(content(p.frames[i], p.major));
    if (p.hasV1) {   // per field: what the ID3v2 tag doesn't say
        if (t.title.empty()) t.title = v1Field(p.v1, 3, 30);
        if (t.artist.empty()) t.artist = v1Field(p.v1, 33, 30);
        if (t.album.empty()) t.album = v1Field(p.v1, 63, 30);
        if (t.year == 0) t.year = yearOf(v1Field(p.v1, 93, 4));
        if (t.track == 0 && p.v1[125] == 0 && p.v1[126] != 0) t.track = p.v1[126];
        if (t.genre.empty()) t.genre = id3v1Genre(p.v1[127]);
    }
    return t;
}

Bytes textBody(const std::string& utf8) {
    const std::wstring w = toWide(utf8);
    Bytes b;
    if (std::all_of(w.begin(), w.end(), [](wchar_t c) { return c < 0x80; })) {
        b.push_back(0);
        for (wchar_t c : w) b.push_back(static_cast<uint8_t>(c));
        return b;
    }
    b = {1, 0xFF, 0xFE};   // UTF-16 with a BOM: valid in v2.3 and v2.4 (and what Windows reads)
    for (wchar_t c : w) {
        b.push_back(static_cast<uint8_t>(c & 0xFF));
        b.push_back(static_cast<uint8_t>(c >> 8));
    }
    return b;
}

Bytes apicBody(const Picture& pic) {
    Bytes b{0};
    append(b, pic.mime.empty() ? mimeOf(pic.data) : pic.mime);
    b.push_back(0);
    b.push_back(3);   // front cover
    b.push_back(0);   // empty description
    append(b, pic.data);
    return b;
}

std::array<uint8_t, 128> updatedV1(const std::array<uint8_t, 128>& v1, const Edit& e, const Tags& next) {
    auto b = v1;
    const auto put = [&](size_t at, size_t len, const std::string& utf8) {
        const std::string l = toLatin1(utf8);
        for (size_t i = 0; i < len; ++i) b[at + i] = i < l.size() ? static_cast<uint8_t>(l[i]) : 0;
    };
    if (e.title) put(3, 30, next.title);
    if (e.artist) put(33, 30, next.artist);
    if (e.album) put(63, 30, next.album);
    if (e.year) put(93, 4, next.year > 0 ? std::to_string(next.year) : std::string());
    if (e.track) {
        const bool v11 = b[125] == 0 || std::all_of(b.begin() + 97, b.begin() + 127, [](uint8_t c) { return c == 0; });
        if (v11) {
            b[125] = 0;
            b[126] = static_cast<uint8_t>(next.track > 0 && next.track <= 255 ? next.track : 0);
        }
    }
    if (e.genre) {
        const int g = id3v1GenreIndex(next.genre);
        b[127] = static_cast<uint8_t>(g >= 0 ? g : 255);
    }
    return b;
}

// The frame ids a field lives in.
std::vector<std::string> idsOf(Field f) {
    switch (f) {
    case kTitle: return {"TIT2"};
    case kArtist: return {"TPE1"};
    case kAlbumArtist: return {"TPE2"};
    case kAlbum: return {"TALB"};
    case kTrack: return {"TRCK"};
    case kDisc: return {"TPOS"};
    case kYear: return {"TYER", "TDRC"};
    case kGenre: return {"TCON"};
    default: return {};
    }
}

bool editedFrame(const Frame& f, const Edit& e) {
    for (int k = 0; k < kFieldCount; ++k)
        if (edits(e, static_cast<Field>(k)))
            for (const auto& id : idsOf(static_cast<Field>(k)))
                if (f.id == id) return true;
    return false;
}

// Pictures that the cover edit replaces / removes: every picture (Remove), or the front covers and the picture shown
// as the cover (Set).
std::vector<char> coverTargets(const std::vector<Frame>& frames, int major, Edit::Cover mode) {
    std::vector<char> hit(frames.size(), 0);
    if (mode == Edit::Cover::Keep) return hit;
    const int shown = coverFrame(frames, major);
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].id != "APIC") continue;
        if (mode == Edit::Cover::Remove) {
            hit[i] = 1;
            continue;
        }
        const auto pic = apic(content(frames[i], major));
        hit[i] = static_cast<int>(i) == shown || (pic && pic->type == 3);
    }
    return hit;
}

// Everything the edit must leave alone: the other frames (byte for byte), and the ID3v1 tag as the edit leaves it.
std::vector<Bytes> preserved(const Parsed& p, const Edit& e) {
    std::vector<Bytes> out;
    const auto pics = coverTargets(p.frames, p.major, e.cover);
    for (size_t i = 0; i < p.frames.size(); ++i) {
        const Frame& f = p.frames[i];
        if (editedFrame(f, e) || pics[i]) continue;
        Bytes b(f.id.begin(), f.id.end());
        b.push_back(f.flags[0]);
        b.push_back(f.flags[1]);
        append(b, f.body);
        out.push_back(std::move(b));
    }
    if (p.hasV1) {
        const auto v1 = updatedV1(p.v1, e, applied(tagsOf(p), e));
        out.emplace_back(v1.begin(), v1.end());
    }
    return out;
}

Ranges payload(const Parsed& p) { return {{p.tagEnd, p.audioEnd}}; }

Plan plan(const Parsed& p, const Edit& e, const Tags& current) {
    if (!p.unsupported.empty()) fail(Status::Unsupported, p.unsupported);
    if (p.junk) fail(Status::Corrupt, "the ID3v2 tag holds data that is neither a frame nor padding");
    const Tags next = applied(current, e);
    const int major = p.major;

    const std::vector<Frame>& frames = p.frames;

    // The new value of an edited frame id ("" = remove it). `existing`: its old text (dates keep their month / day).
    const auto value = [&](const std::string& id, const std::string& existing) -> std::string {
        if (id == "TIT2") return next.title;
        if (id == "TPE1") return next.artist;
        if (id == "TPE2") return next.albumArtist;
        if (id == "TALB") return next.album;
        if (id == "TCON") return next.genre;
        if (id == "TRCK")
            return next.track <= 0 ? "" : next.trackTotal > 0 ? std::to_string(next.track) + "/" + std::to_string(next.trackTotal)
                                                               : std::to_string(next.track);
        if (id == "TPOS")
            return next.disc <= 0 ? "" : next.discTotal > 0 ? std::to_string(next.disc) + "/" + std::to_string(next.discTotal)
                                                             : std::to_string(next.disc);
        if (id == "TYER") return next.year > 0 ? withYear("", next.year) : "";
        if (id == "TDRC") return next.year > 0 ? withYear(existing, next.year) : "";
        return {};
    };
    // Frames created when the tag has none: the version's own year frame only.
    const auto createdIds = [&](Field f) -> std::vector<std::string> {
        if (f == kYear) return {major == 4 ? "TDRC" : "TYER"};
        return idsOf(f);
    };

    const auto pics = coverTargets(frames, major, e.cover);
    std::vector<Frame> out;
    std::set<std::string> placed;
    bool picPlaced = false;
    const auto newPicture = [&] {
        picPlaced = true;
        if (e.cover == Edit::Cover::Set && !e.picture.data.empty()) out.push_back({"APIC", {0, 0}, apicBody(e.picture)});
    };
    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame& f = frames[i];
        if (pics[i]) {
            if (!picPlaced) newPicture();
            continue;
        }
        if (!editedFrame(f, e)) {
            out.push_back(f);
            continue;
        }
        if (!placed.insert(f.id).second) continue;   // a duplicate of an edited frame: dropped
        const auto old = textValues(content(f, major));
        const std::string v = value(f.id, old.empty() ? std::string() : old[0]);
        if (!v.empty()) out.push_back({f.id, {0, 0}, textBody(v)});
    }
    for (int k = 0; k < kFieldCount; ++k) {
        const auto field = static_cast<Field>(k);
        if (!edits(e, field)) continue;
        bool any = false;
        for (const auto& id : idsOf(field)) any = any || placed.contains(id);
        if (any) continue;
        for (const auto& id : createdIds(field))
            if (const std::string v = value(id, {}); !v.empty()) out.push_back({id, {0, 0}, textBody(v)});
    }
    if (!picPlaced) newPicture();

    // Serialize: no unsynchronisation, no extended header, no footer.
    Bytes body;
    for (const auto& f : out) {
        if (f.body.size() > 0x0FFFFFFF) fail(Status::TooLarge, "ID3 frame too large");
        append(body, f.id);
        if (major == 4) putSyncsafe(body, static_cast<uint32_t>(f.body.size()));
        else putBE32(body, static_cast<uint32_t>(f.body.size()));
        body.push_back(f.flags[0]);
        body.push_back(f.flags[1]);
        append(body, f.body);
    }
    // Same size as before when the old tag has room (and not much more): the audio stays where it was.
    const uint64_t room = p.tagEnd;
    const uint64_t need = 10 + body.size();
    const uint64_t padding = room >= need && room - need <= 64 * 1024 ? room - need : kId3Padding;
    if (body.size() + padding > 0x0FFFFFFF) fail(Status::TooLarge, "ID3 tag too large");
    Bytes tag{'I', 'D', '3', static_cast<uint8_t>(major), 0, 0};
    putSyncsafe(tag, static_cast<uint32_t>(body.size() + padding));
    append(tag, body);
    tag.resize(tag.size() + static_cast<size_t>(padding), 0);

    Plan plan;
    plan.push_back(literal(std::move(tag)));
    plan.push_back(copyRange(p.tagEnd, p.audioEnd));
    if (p.hasV1) {
        const auto v1 = updatedV1(p.v1, e, next);
        plan.push_back(literal(Bytes(v1.begin(), v1.end())));
    }
    return plan;
}

} // namespace id3

// =====================================================================================================================
// FLAC
namespace flac {

enum : uint8_t { kStreamInfo = 0, kPadding = 1, kVorbis = 4, kPicture = 6 };

struct Block {
    uint8_t type = 0;
    Bytes data;               // empty for padding (only its length counts)
    uint32_t length = 0;
};

struct Parsed {
    uint64_t metaStart = 0;   // right after "fLaC" (anything before it, e.g. an ID3 tag, is kept as it is)
    uint64_t audioStart = 0;
    std::vector<Block> blocks;
    uint64_t fileSize = 0;
};

uint64_t id3Prefix(Source& s) {
    uint8_t h[10];
    if (s.size() < 10 || !s.read(0, h, 10) || std::memcmp(h, "ID3", 3) != 0) return 0;
    return 10 + uint64_t(syncsafe(h + 6)) + ((h[5] & 0x10) ? 10 : 0);
}

Parsed parse(Source& s) {
    Parsed p;
    p.fileSize = s.size();
    const uint64_t start = id3Prefix(s);
    uint8_t magic[4];
    if (!s.read(start, magic, 4) || std::memcmp(magic, "fLaC", 4) != 0) fail(Status::Corrupt, "no FLAC stream marker");
    p.metaStart = start + 4;
    uint64_t pos = p.metaStart, total = 0;
    for (int guard = 0;; ++guard) {
        if (guard > 4096) fail(Status::Corrupt, "too many FLAC metadata blocks");
        uint8_t h[4];
        if (!s.read(pos, h, 4)) fail(Status::Corrupt, "truncated FLAC metadata");
        Block b;
        b.type = h[0] & 0x7F;
        b.length = be24(h + 1);
        if (b.type == 127) fail(Status::Corrupt, "invalid FLAC metadata block");
        if (pos + 4 + b.length > s.size()) fail(Status::Corrupt, "FLAC metadata runs past the end of the file");
        total += b.length;
        if (total > kMaxTagBytes) fail(Status::TooLarge, "FLAC metadata too large");
        if (b.type != kPadding) b.data = s.read(pos + 4, b.length);
        p.blocks.push_back(std::move(b));
        pos += 4 + uint64_t(p.blocks.back().length);
        if (h[0] & 0x80) break;
    }
    p.audioStart = pos;
    if (p.blocks.empty() || p.blocks[0].type != kStreamInfo || p.blocks[0].length != 34)
        fail(Status::Corrupt, "FLAC stream without STREAMINFO");
    return p;
}

struct Vorbis {
    std::string vendor;
    std::vector<std::string> comments;
};

std::optional<Vorbis> parseVorbis(const Bytes& d) {
    Vorbis v;
    size_t i = 0;
    const auto u32 = [&](uint32_t& out) {
        if (i + 4 > d.size()) return false;
        out = le32(&d[i]);
        i += 4;
        return true;
    };
    uint32_t n = 0;
    if (!u32(n) || n > d.size() - i) return std::nullopt;
    v.vendor.assign(reinterpret_cast<const char*>(&d[i]), n);
    i += n;
    uint32_t count = 0;
    if (!u32(count)) return std::nullopt;
    for (uint32_t k = 0; k < count; ++k) {
        if (!u32(n) || n > d.size() - i) return std::nullopt;
        v.comments.emplace_back(reinterpret_cast<const char*>(&d[i]), n);
        i += n;
    }
    return v;
}

Bytes serializeVorbis(const Vorbis& v) {
    Bytes d;
    putLE32(d, static_cast<uint32_t>(v.vendor.size()));
    append(d, v.vendor);
    putLE32(d, static_cast<uint32_t>(v.comments.size()));
    for (const auto& c : v.comments) {
        putLE32(d, static_cast<uint32_t>(c.size()));
        append(d, c);
    }
    return d;
}

std::string keyOf(const std::string& comment) { return upperAscii(comment.substr(0, comment.find('='))); }
std::string valueOf(const std::string& comment) {
    const size_t eq = comment.find('=');
    return eq == std::string::npos ? std::string() : trim(comment.substr(eq + 1));
}

std::vector<std::string> keysOf(Field f) {
    switch (f) {
    case kTitle: return {"TITLE"};
    case kArtist: return {"ARTIST"};
    case kAlbumArtist: return {"ALBUMARTIST", "ALBUM ARTIST", "ALBUM_ARTIST"};
    case kAlbum: return {"ALBUM"};
    case kTrack: return {"TRACKNUMBER", "TRACKTOTAL", "TOTALTRACKS"};
    case kDisc: return {"DISCNUMBER", "DISCTOTAL", "TOTALDISCS"};
    case kYear: return {"DATE", "YEAR"};
    case kGenre: return {"GENRE"};
    default: return {};
    }
}

bool editedComment(const std::string& key, const Edit& e) {
    for (int k = 0; k < kFieldCount; ++k)
        if (edits(e, static_cast<Field>(k)))
            for (const auto& id : keysOf(static_cast<Field>(k)))
                if (key == id) return true;
    return false;
}

std::optional<Picture> pictureOf(const Bytes& d) {
    if (d.size() < 32) return std::nullopt;
    size_t i = 0;
    const auto u32 = [&](uint32_t& v) {
        if (i + 4 > d.size()) return false;
        v = be32(&d[i]);
        i += 4;
        return true;
    };
    uint32_t type = 0, n = 0;
    if (!u32(type) || !u32(n) || n > d.size() - i) return std::nullopt;
    std::string mime(reinterpret_cast<const char*>(&d[i]), n);
    i += n;
    if (!u32(n) || n > d.size() - i) return std::nullopt;
    i += n;   // description
    if (i + 16 > d.size()) return std::nullopt;
    i += 16;  // width, height, depth, colors
    if (!u32(n) || n > d.size() - i || n == 0) return std::nullopt;
    Picture pic;
    pic.type = static_cast<int>(type);
    pic.data.assign(d.begin() + static_cast<std::ptrdiff_t>(i), d.begin() + static_cast<std::ptrdiff_t>(i + n));
    const std::string sniffed = mimeOf(pic.data);
    pic.mime = !sniffed.empty() ? sniffed : lowerAscii(mime);
    return pic;
}

Bytes pictureBlock(const Picture& pic) {
    const auto [w, h] = imageSize(pic.data);
    Bytes d;
    putBE32(d, 3);
    const std::string mime = pic.mime.empty() ? mimeOf(pic.data) : pic.mime;
    putBE32(d, static_cast<uint32_t>(mime.size()));
    append(d, mime);
    putBE32(d, 0);   // description
    putBE32(d, static_cast<uint32_t>(w));
    putBE32(d, static_cast<uint32_t>(h));
    putBE32(d, 24);  // color depth
    putBE32(d, 0);   // indexed colors
    putBE32(d, static_cast<uint32_t>(pic.data.size()));
    append(d, pic.data);
    return d;
}

int coverBlock(const std::vector<Block>& blocks) {
    int first = -1;
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
        if (blocks[i].type != kPicture) continue;
        const auto pic = pictureOf(blocks[i].data);
        if (!pic) continue;
        if (pic->type == 3) return i;
        if (first < 0) first = i;
    }
    return first;
}

const Block* vorbisBlock(const Parsed& p) {
    for (const auto& b : p.blocks)
        if (b.type == kVorbis) return &b;
    return nullptr;
}

Tags tagsOf(const Parsed& p) {
    Tags t;
    if (const Block* b = vorbisBlock(p)) {
        if (const auto v = parseVorbis(b->data)) {
            std::vector<std::string> artists, genres;
            std::string date, year, albumArtist, albumArtistAlt;
            int trackTotal = 0, discTotal = 0;
            for (const auto& c : v->comments) {
                const std::string k = keyOf(c), val = valueOf(c);
                if (val.empty()) continue;
                if (k == "TITLE" && t.title.empty()) t.title = val;
                else if (k == "ARTIST") artists.push_back(val);
                else if (k == "ALBUM" && t.album.empty()) t.album = val;
                else if (k == "ALBUMARTIST" && albumArtist.empty()) albumArtist = val;
                else if ((k == "ALBUM ARTIST" || k == "ALBUM_ARTIST") && albumArtistAlt.empty()) albumArtistAlt = val;
                else if (k == "GENRE") genres.push_back(val);
                else if (k == "TRACKNUMBER" && t.track == 0) std::tie(t.track, t.trackTotal) = numberPair(val);
                else if ((k == "TRACKTOTAL" || k == "TOTALTRACKS") && trackTotal == 0) trackTotal = leadingInt(val);
                else if (k == "DISCNUMBER" && t.disc == 0) std::tie(t.disc, t.discTotal) = numberPair(val);
                else if ((k == "DISCTOTAL" || k == "TOTALDISCS") && discTotal == 0) discTotal = leadingInt(val);
                else if (k == "DATE" && date.empty()) date = val;
                else if (k == "YEAR" && year.empty()) year = val;
            }
            t.artist = id3::joined(artists);
            t.genre = id3::joined(genres);
            t.albumArtist = !albumArtist.empty() ? albumArtist : albumArtistAlt;
            if (t.trackTotal == 0) t.trackTotal = trackTotal;
            if (t.discTotal == 0) t.discTotal = discTotal;
            if (t.track == 0) t.trackTotal = 0;
            if (t.disc == 0) t.discTotal = 0;
            t.year = yearOf(date) ? yearOf(date) : yearOf(year);
        }
    }
    if (const int i = coverBlock(p.blocks); i >= 0) t.cover = pictureOf(p.blocks[static_cast<size_t>(i)].data);
    return t;
}

std::vector<char> coverTargets(const std::vector<Block>& blocks, Edit::Cover mode) {
    std::vector<char> hit(blocks.size(), 0);
    if (mode == Edit::Cover::Keep) return hit;
    const int shown = coverBlock(blocks);
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].type != kPicture) continue;
        if (mode == Edit::Cover::Remove) {
            hit[i] = 1;
            continue;
        }
        const auto pic = pictureOf(blocks[i].data);
        hit[i] = static_cast<int>(i) == shown || (pic && pic->type == 3);
    }
    return hit;
}

// The vendor string first ("" when there is no comment block: a new block gets an empty one), then the comments that
// aren't edited, then every other block that isn't padding or a picture the edit replaces.
std::vector<Bytes> preserved(const Parsed& p, const Edit& e) {
    std::vector<Bytes> out{Bytes{}};
    const auto pics = coverTargets(p.blocks, e.cover);
    bool vorbis = false;
    std::vector<Bytes> blocks;
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        const Block& b = p.blocks[i];
        if (b.type == kPadding || pics[i]) continue;
        if (b.type == kVorbis) {
            const auto v = parseVorbis(b.data);
            if (!v || vorbis) continue;
            vorbis = true;
            out[0].assign(v->vendor.begin(), v->vendor.end());
            for (const auto& c : v->comments)
                if (!editedComment(keyOf(c), e)) out.push_back(Bytes(c.begin(), c.end()));
            continue;
        }
        Bytes raw{b.type};
        append(raw, b.data);
        blocks.push_back(std::move(raw));
    }
    out.insert(out.end(), blocks.begin(), blocks.end());
    return out;
}

Ranges payload(const Parsed& p) { return {{p.audioStart, p.fileSize}}; }

Vorbis editedVorbis(Vorbis v, const Edit& e, const Tags& next) {
    for (int k = 0; k < kFieldCount; ++k) {
        const auto field = static_cast<Field>(k);
        if (!edits(e, field)) continue;
        const auto keys = keysOf(field);
        // Old forms worth keeping: "3/12" in TRACKNUMBER, a full date in DATE, whether YEAR was there.
        bool slashForm = false, hadYear = false;
        std::string oldDate;
        size_t at = v.comments.size();
        for (size_t i = 0; i < v.comments.size();) {
            const std::string key = keyOf(v.comments[i]);
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                ++i;
                continue;
            }
            if ((key == "TRACKNUMBER" || key == "DISCNUMBER") && valueOf(v.comments[i]).find('/') != std::string::npos)
                slashForm = true;
            if (key == "DATE" && oldDate.empty()) oldDate = valueOf(v.comments[i]);
            if (key == "YEAR") hadYear = true;
            at = std::min(at, i);
            v.comments.erase(v.comments.begin() + static_cast<std::ptrdiff_t>(i));
        }
        std::vector<std::string> add;
        const auto number = [&](const char* key, const char* totalKey, int n, int total) {
            if (n <= 0) return;   // removing the number removes its total too
            if (slashForm && total > 0) add.push_back(std::string(key) + "=" + std::to_string(n) + "/" + std::to_string(total));
            else {
                add.push_back(std::string(key) + "=" + std::to_string(n));
                if (total > 0) add.push_back(std::string(totalKey) + "=" + std::to_string(total));
            }
        };
        switch (field) {
        case kTitle:
            if (!next.title.empty()) add.push_back("TITLE=" + next.title);
            break;
        case kArtist: {
            const std::string list = next.artist;
            for (size_t s = 0; s <= list.size() && !list.empty();) {
                const size_t semi = list.find(';', s);
                const std::string one = trim(list.substr(s, semi == std::string::npos ? std::string::npos : semi - s));
                if (!one.empty()) add.push_back("ARTIST=" + one);
                if (semi == std::string::npos) break;
                s = semi + 1;
            }
            break;
        }
        case kAlbumArtist:
            if (!next.albumArtist.empty()) add.push_back("ALBUMARTIST=" + next.albumArtist);
            break;
        case kAlbum:
            if (!next.album.empty()) add.push_back("ALBUM=" + next.album);
            break;
        case kTrack: number("TRACKNUMBER", "TRACKTOTAL", next.track, next.trackTotal); break;
        case kDisc: number("DISCNUMBER", "DISCTOTAL", next.disc, next.discTotal); break;
        case kYear:
            if (next.year > 0) {
                // A full date stays in DATE; Windows' FLAC handler (and so the library) only reads a bare year from
                // DATE or YEAR, so a full date gets a YEAR next to it.
                const std::string date = withYear(oldDate, next.year);
                add.push_back("DATE=" + date);
                if (hadYear || date.size() != 4) add.push_back("YEAR=" + withYear("", next.year));
            }
            break;
        case kGenre:
            if (!next.genre.empty()) add.push_back("GENRE=" + next.genre);
            break;
        default: break;
        }
        v.comments.insert(v.comments.begin() + static_cast<std::ptrdiff_t>(std::min(at, v.comments.size())), add.begin(), add.end());
    }
    return v;
}

Plan plan(const Parsed& p, const Edit& e, const Tags& current) {
    const Tags next = applied(current, e);
    const auto pics = coverTargets(p.blocks, e.cover);
    std::vector<Block> out;
    bool vorbisDone = false, picPlaced = false;
    const auto newPicture = [&] {
        picPlaced = true;
        if (e.cover == Edit::Cover::Set && !e.picture.data.empty()) out.push_back({kPicture, pictureBlock(e.picture), 0});
    };
    const bool textEdit = e.title || e.artist || e.album || e.albumArtist || e.genre || e.track || e.disc || e.year;
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        const Block& b = p.blocks[i];
        if (b.type == kPadding) continue;
        if (pics[i]) {
            if (!picPlaced) newPicture();
            continue;
        }
        if (b.type == kVorbis) {
            if (vorbisDone) continue;   // a second comment block is invalid: dropped
            vorbisDone = true;
            if (!textEdit) {
                out.push_back(b);
                continue;
            }
            const auto v = parseVorbis(b.data);
            if (!v) fail(Status::Corrupt, "malformed Vorbis comment block");
            out.push_back({kVorbis, serializeVorbis(editedVorbis(*v, e, next)), 0});
            continue;
        }
        out.push_back(b);
    }
    if (!vorbisDone && textEdit) {   // a new comment block right after STREAMINFO
        const Vorbis v = editedVorbis({}, e, next);
        if (!v.comments.empty()) out.insert(out.begin() + 1, Block{kVorbis, serializeVorbis(v), 0});
    }
    if (!picPlaced) newPicture();

    uint64_t used = 0;
    for (auto& b : out) {
        if (b.data.size() > 0xFFFFFF) fail(Status::TooLarge, "FLAC metadata block too large");
        b.length = static_cast<uint32_t>(b.data.size());
        used += 4 + b.length;
    }
    // The padding absorbs the change when it fits: the audio frames stay where they were. Otherwise the metadata is
    // rewritten with fresh padding.
    const uint64_t room = p.audioStart - p.metaStart;
    int64_t padding = -1;   // -1 = no padding block
    if (room == used) padding = -1;
    else if (room >= used + 4 && room - used - 4 <= (1u << 20)) padding = static_cast<int64_t>(room - used - 4);
    else padding = kFlacPadding;
    if (padding >= 0) out.push_back({kPadding, {}, static_cast<uint32_t>(padding)});

    Bytes meta;
    for (size_t i = 0; i < out.size(); ++i) {
        const Block& b = out[i];
        meta.push_back(static_cast<uint8_t>(b.type | (i + 1 == out.size() ? 0x80 : 0)));
        putBE24(meta, b.length);
        if (b.type == kPadding) meta.resize(meta.size() + b.length, 0);
        else append(meta, b.data);
    }
    Plan plan;
    plan.push_back(copyRange(0, p.metaStart));   // an ID3 tag in front (kept) + "fLaC"
    plan.push_back(literal(std::move(meta)));
    plan.push_back(copyRange(p.audioStart, p.fileSize));
    return plan;
}

} // namespace flac

// =====================================================================================================================
// MP4 / M4A
namespace mp4 {

struct Box {
    uint64_t start = 0, content = 0, end = 0;   // absolute (file) or buffer offsets
    char type[4] = {};
    bool is(const char* t) const { return std::memcmp(type, t, 4) == 0; }
};

// Box at `pos` of a buffer, inside [pos, limit).
bool boxAt(const Bytes& b, uint64_t pos, uint64_t limit, Box& out) {
    if (pos + 8 > limit) return false;
    uint64_t size = be32(&b[pos]);
    uint64_t header = 8;
    if (size == 1) {
        if (pos + 16 > limit) return false;
        size = be64(&b[pos + 8]);
        header = 16;
    } else if (size == 0) {
        size = limit - pos;
    }
    if (size < header || pos + size > limit) return false;
    out.start = pos;
    out.content = pos + header;
    out.end = pos + size;
    std::memcpy(out.type, &b[pos + 4], 4);
    return true;
}

// The children of [from, to); fails (strict) when they don't tile it. A few zero bytes at the end are fine (QuickTime
// ends user data lists with a 32-bit zero).
std::vector<Box> children(const Bytes& b, uint64_t from, uint64_t to, bool strict = true) {
    std::vector<Box> out;
    Box x;
    uint64_t pos = from;
    while (pos < to) {
        if (to - pos < 8 && std::all_of(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(to),
                                        [](uint8_t c) { return c == 0; }))
            break;
        if (!boxAt(b, pos, to, x)) {
            if (strict) fail(Status::Corrupt, "malformed MP4 box");
            break;
        }
        out.push_back(x);
        pos = x.end;
    }
    return out;
}

const Box* find(const std::vector<Box>& v, const char* type) {
    for (const auto& b : v)
        if (b.is(type)) return &b;
    return nullptr;
}

Bytes slice(const Bytes& b, const Box& x) {
    return Bytes(b.begin() + static_cast<std::ptrdiff_t>(x.start), b.begin() + static_cast<std::ptrdiff_t>(x.end));
}

Bytes makeBox(const char* type, const Bytes& content) {
    Bytes b;
    if (content.size() + 8 > 0xFFFFFFFFull) fail(Status::TooLarge, "MP4 box too large");
    putBE32(b, static_cast<uint32_t>(content.size() + 8));
    append(b, std::string_view(type, 4));
    append(b, content);
    return b;
}

struct Parsed {
    std::vector<Box> top;      // file offsets
    size_t moovIndex = 0;
    Bytes moov;                // the whole moov box
    uint64_t fileSize = 0;
    // Inside `moov` (buffer offsets): where the metadata lives.
    std::vector<Box> moovKids;
    std::optional<Box> udta, meta, ilst;
    bool metaInMoov = false;   // moov/meta instead of moov/udta/meta
    uint64_t metaContent = 0;  // after meta's version / flags (QuickTime files have none)
    std::vector<Box> items;    // ilst children
};

Parsed parse(Source& s) {
    Parsed p;
    p.fileSize = s.size();
    bool haveMoov = false;
    uint8_t h[16];
    for (uint64_t pos = 0; pos < s.size();) {
        if (p.top.size() > 200'000) fail(Status::Corrupt, "too many MP4 boxes");
        if (pos + 8 > s.size() || !s.read(pos, h, 8)) fail(Status::Corrupt, "truncated MP4 box header");
        uint64_t size = be32(h), header = 8;
        if (size == 1) {
            if (pos + 16 > s.size() || !s.read(pos + 8, h + 8, 8)) fail(Status::Corrupt, "truncated MP4 box header");
            size = be64(h + 8);
            header = 16;
        } else if (size == 0) {
            size = s.size() - pos;
        }
        if (size < header || size > s.size() - pos) fail(Status::Corrupt, "an MP4 box runs past the end of the file");
        Box b;
        b.start = pos;
        b.content = pos + header;
        b.end = pos + size;
        std::memcpy(b.type, h + 4, 4);
        if (b.is("moov")) {
            if (haveMoov) fail(Status::Corrupt, "two moov boxes");
            haveMoov = true;
            p.moovIndex = p.top.size();
        }
        p.top.push_back(b);
        pos = b.end;
    }
    if (p.top.empty() || !p.top[0].is("ftyp")) fail(Status::Corrupt, "no ftyp box");
    if (!haveMoov) fail(Status::Corrupt, "no moov box");
    const Box& mv = p.top[p.moovIndex];
    if (mv.end - mv.start > kMaxMoovBytes) fail(Status::TooLarge, "moov box too large");
    p.moov = s.read(mv.start, static_cast<size_t>(mv.end - mv.start));
    Box root;
    boxAt(p.moov, 0, p.moov.size(), root);
    p.moovKids = children(p.moov, root.content, root.end);
    if (const Box* u = find(p.moovKids, "udta")) {
        p.udta = *u;
        const auto kids = children(p.moov, u->content, u->end);
        if (const Box* m = find(kids, "meta")) p.meta = *m;
    }
    if (!p.meta)
        if (const Box* m = find(p.moovKids, "meta")) {
            p.meta = *m;
            p.metaInMoov = true;
        }
    if (p.meta) {
        // A full box (version / flags) in iTunes files; QuickTime-style files start with a child box right away.
        p.metaContent = p.meta->content + 4;
        Box probe;
        if (boxAt(p.moov, p.meta->content, p.meta->end, probe) && probe.is("hdlr")) p.metaContent = p.meta->content;
        if (p.metaContent > p.meta->end) fail(Status::Corrupt, "malformed meta box");
        const auto kids = children(p.moov, p.metaContent, p.meta->end);
        if (const Box* l = find(kids, "ilst")) {
            p.ilst = *l;
            p.items = children(p.moov, l->content, l->end);
        }
    }
    return p;
}

// The first data atom of an item: {type, payload}.
std::optional<std::pair<uint32_t, Bytes>> dataOf(const Bytes& moov, const Box& item) {
    for (const auto& c : children(moov, item.content, item.end, false)) {
        if (!c.is("data") || c.end - c.content < 8) continue;
        const uint32_t type = be24(&moov[c.content + 1]);
        return std::pair<uint32_t, Bytes>{type, Bytes(moov.begin() + static_cast<std::ptrdiff_t>(c.content + 8),
                                                      moov.begin() + static_cast<std::ptrdiff_t>(c.end))};
    }
    return std::nullopt;
}

std::string text(const Bytes& d) { return trim(std::string(d.begin(), d.end())); }

std::string keyOf(const Box& b) { return std::string(b.type, 4); }

const std::string kNam = "\xA9nam", kArt = "\xA9" "ART", kAlb = "\xA9" "alb", kAart = "aART", kGen = "\xA9gen",
                  kGnre = "gnre", kDay = "\xA9" "day", kTrkn = "trkn", kDisk = "disk", kCovr = "covr";

std::vector<std::string> keysOf(Field f) {
    switch (f) {
    case kTitle: return {kNam};
    case kArtist: return {kArt};
    case kAlbumArtist: return {kAart};
    case kAlbum: return {kAlb};
    case kTrack: return {kTrkn};
    case kDisc: return {kDisk};
    case kYear: return {kDay};
    case kGenre: return {kGen, kGnre};
    default: return {};
    }
}

bool editedItem(const std::string& key, const Edit& e) {
    if (key == kCovr) return e.cover != Edit::Cover::Keep;
    for (int k = 0; k < kFieldCount; ++k)
        if (edits(e, static_cast<Field>(k)))
            for (const auto& id : keysOf(static_cast<Field>(k)))
                if (key == id) return true;
    return false;
}

Tags tagsOf(const Parsed& p) {
    Tags t;
    for (const auto& item : p.items) {
        const std::string key = keyOf(item);
        const auto d = dataOf(p.moov, item);
        if (!d) continue;
        const auto& [type, v] = *d;
        if (key == kNam && t.title.empty()) t.title = text(v);
        else if (key == kArt && t.artist.empty()) t.artist = text(v);
        else if (key == kAlb && t.album.empty()) t.album = text(v);
        else if (key == kAart && t.albumArtist.empty()) t.albumArtist = text(v);
        else if (key == kGen && type == 1) t.genre = text(v);
        else if (key == kGnre && t.genre.empty() && v.size() >= 2) t.genre = id3v1Genre(static_cast<int>(be16(v.data())) - 1);
        else if (key == kDay && t.year == 0) t.year = yearOf(text(v));
        else if (key == kTrkn && v.size() >= 6) {
            t.track = static_cast<int>(be16(&v[2]));
            t.trackTotal = static_cast<int>(be16(&v[4]));
        } else if (key == kDisk && v.size() >= 6) {
            t.disc = static_cast<int>(be16(&v[2]));
            t.discTotal = static_cast<int>(be16(&v[4]));
        } else if (key == kCovr && !t.cover && !v.empty()) {
            Picture pic;
            pic.data = v;
            pic.mime = type == 14 ? "image/png" : type == 13 ? "image/jpeg" : mimeOf(v);
            t.cover = std::move(pic);
        }
    }
    if (t.track == 0) t.trackTotal = 0;
    if (t.disc == 0) t.discTotal = 0;
    return t;
}

Bytes item(const std::string& key, uint32_t type, const Bytes& payload) {
    Bytes data{0};
    putBE24(data, type);
    putBE32(data, 0);   // locale
    append(data, payload);
    return makeBox(key.c_str(), makeBox("data", data));
}
Bytes textItem(const std::string& key, const std::string& v) { return item(key, 1, Bytes(v.begin(), v.end())); }

// stco / co64 chunk offsets of every track (buffer positions of the entries), for patching and probing.
struct Offset {
    size_t at;       // entry position in the buffer
    bool wide;       // co64
};
void chunkOffsets(const Bytes& b, uint64_t from, uint64_t to, std::vector<Offset>& out, int depth = 0) {
    if (depth > 8) return;
    for (const auto& c : children(b, from, to)) {
        if (c.is("trak") || c.is("mdia") || c.is("minf") || c.is("stbl")) chunkOffsets(b, c.content, c.end, out, depth + 1);
        else if ((c.is("stco") || c.is("co64")) && c.end - c.content >= 8) {
            const bool wide = c.is("co64");
            const uint64_t n = be32(&b[c.content + 4]);
            const uint64_t entry = wide ? 8 : 4;
            if (c.content + 8 + n * entry > c.end) fail(Status::Corrupt, "malformed chunk offset box");
            for (uint64_t i = 0; i < n; ++i) out.push_back({static_cast<size_t>(c.content + 8 + i * entry), wide});
        }
    }
}
std::vector<Offset> chunkOffsets(const Bytes& moov) {
    std::vector<Offset> out;
    Box root;
    if (boxAt(moov, 0, moov.size(), root)) chunkOffsets(moov, root.content, root.end, out);
    return out;
}
uint64_t offsetValue(const Bytes& b, const Offset& o) { return o.wide ? be64(&b[o.at]) : be32(&b[o.at]); }

// Absolute offsets outside moov that move with the boxes behind it: tfhd base data offsets (moof) and tfra moof
// offsets (mfra). Returns the patches for the boxes after `from` (the old moov end).
std::vector<Patch> fragmentPatches(Source& s, const Parsed& p, uint64_t threshold, int64_t delta) {
    std::vector<Patch> out;
    const auto shifted = [&](uint64_t v) { return v >= threshold ? static_cast<uint64_t>(static_cast<int64_t>(v) + delta) : v; };
    for (size_t i = p.moovIndex + 1; i < p.top.size(); ++i) {
        const Box& top = p.top[i];
        if (!top.is("moof") && !top.is("mfra")) continue;
        if (top.end - top.start > (64u << 20)) fail(Status::TooLarge, "fragment box too large");
        const Bytes b = s.read(top.start, static_cast<size_t>(top.end - top.start));
        Box root;
        boxAt(b, 0, b.size(), root);
        for (const auto& c : children(b, root.content, root.end)) {
            if (top.is("moof") && c.is("traf")) {
                for (const auto& t : children(b, c.content, c.end)) {
                    if (!t.is("tfhd") || t.end - t.content < 16) continue;
                    if (!(be24(&b[t.content + 1]) & 0x000001)) continue;   // no base data offset: relative to moof
                    const uint64_t v = be64(&b[t.content + 8]);
                    if (shifted(v) == v) continue;
                    Bytes nb(8);
                    writeBE64(nb.data(), shifted(v));
                    out.push_back({top.start + t.content + 8, std::move(nb)});
                }
            } else if (top.is("mfra") && c.is("tfra") && c.end - c.content >= 16) {
                const uint8_t version = b[c.content];
                const uint32_t sizes = be32(&b[c.content + 8]);
                const uint32_t n = be32(&b[c.content + 12]);
                const uint64_t extra = ((sizes >> 4) & 3) + 1 + ((sizes >> 2) & 3) + 1 + (sizes & 3) + 1;
                const uint64_t entry = (version == 1 ? 16 : 8) + extra;
                if (c.content + 16 + uint64_t(n) * entry > c.end) fail(Status::Corrupt, "malformed tfra box");
                for (uint32_t k = 0; k < n; ++k) {
                    const uint64_t at = c.content + 16 + k * entry + (version == 1 ? 8 : 4);
                    const uint64_t v = version == 1 ? be64(&b[at]) : be32(&b[at]);
                    const uint64_t nv = shifted(v);
                    if (nv == v) continue;
                    if (version != 1 && nv > 0xFFFFFFFFull) fail(Status::TooLarge, "fragment offset overflow");
                    Bytes nb(version == 1 ? 8 : 4);
                    if (version == 1) writeBE64(nb.data(), nv);
                    else writeBE32(nb.data(), static_cast<uint32_t>(nv));
                    out.push_back({top.start + at, std::move(nb)});
                }
            }
        }
    }
    return out;
}

std::vector<Bytes> preserved(const Parsed& p, const Edit& e) {
    std::vector<Bytes> out;
    // moov's other children; a track's chunk offsets may move, so they are compared as zeros.
    for (const auto& c : p.moovKids) {
        if (c.is("udta") || (p.metaInMoov && c.is("meta"))) continue;
        Bytes raw = slice(p.moov, c);
        if (c.is("trak")) {
            std::vector<Offset> offs;
            chunkOffsets(raw, 0, raw.size(), offs);
            for (const auto& o : offs) std::fill_n(raw.begin() + static_cast<std::ptrdiff_t>(o.at), o.wide ? 8 : 4, uint8_t{0});
        }
        out.push_back(std::move(raw));
    }
    if (p.udta)
        for (const auto& c : children(p.moov, p.udta->content, p.udta->end))
            if (!c.is("meta")) out.push_back(slice(p.moov, c));
    for (const auto& it : p.items)
        if (!editedItem(keyOf(it), e)) out.push_back(slice(p.moov, it));
    return out;
}

Ranges payload(const Parsed& p) {
    Ranges r;
    for (const auto& b : p.top)
        if (b.is("mdat")) r.emplace_back(b.content, b.end);
    return r;
}

Bytes freeBox(uint64_t size) {
    Bytes b;
    putBE32(b, static_cast<uint32_t>(size));
    append(b, "free");
    b.resize(static_cast<size_t>(size), 0);
    return b;
}

Plan plan(Source& s, const Parsed& p, const Edit& e, const Tags& current) {
    const Tags next = applied(current, e);
    // ---- the new ilst
    Bytes items;
    std::set<std::string> placed;
    const auto newItems = [&](const std::string& key) -> Bytes {
        std::string existingDay;
        if (key == kDay)
            for (const auto& it : p.items)
                if (keyOf(it) == kDay)
                    if (const auto d = dataOf(p.moov, it)) {
                        existingDay = text(d->second);
                        break;
                    }
        if (key == kNam) return next.title.empty() ? Bytes{} : textItem(kNam, next.title);
        if (key == kArt) return next.artist.empty() ? Bytes{} : textItem(kArt, next.artist);
        if (key == kAlb) return next.album.empty() ? Bytes{} : textItem(kAlb, next.album);
        if (key == kAart) return next.albumArtist.empty() ? Bytes{} : textItem(kAart, next.albumArtist);
        if (key == kGen) return next.genre.empty() ? Bytes{} : textItem(kGen, next.genre);
        if (key == kGnre) return {};   // a numeric genre becomes text
        if (key == kDay) return next.year <= 0 ? Bytes{} : textItem(kDay, withYear(existingDay, next.year));
        if (key == kTrkn || key == kDisk) {
            const int n = key == kTrkn ? next.track : next.disc, total = key == kTrkn ? next.trackTotal : next.discTotal;
            if (n <= 0) return {};
            if (n > 0xFFFF || total > 0xFFFF) fail(Status::TooLarge, "track / disc number out of range");
            Bytes v{0, 0};
            putBE16(v, static_cast<uint32_t>(n));
            putBE16(v, static_cast<uint32_t>(total));
            if (key == kTrkn) putBE16(v, 0);
            return item(key, 0, v);
        }
        if (key == kCovr) {
            if (e.cover != Edit::Cover::Set || e.picture.data.empty()) return {};
            const std::string mime = e.picture.mime.empty() ? mimeOf(e.picture.data) : e.picture.mime;
            return item(kCovr, mime == "image/png" ? 14 : 13, e.picture.data);
        }
        return {};
    };
    // Which new key a field's group places (the genre group writes one text item).
    const auto groupOf = [&](const std::string& key) -> std::string { return key == kGnre ? kGen : key; };
    for (const auto& it : p.items) {
        const std::string key = keyOf(it);
        if (!editedItem(key, e)) {
            append(items, slice(p.moov, it));
            continue;
        }
        if (!placed.insert(groupOf(key)).second) continue;
        append(items, newItems(groupOf(key)));
    }
    for (const std::string& key : {kNam, kArt, kAart, kAlb, kTrkn, kDisk, kDay, kGen, kCovr}) {
        if (!editedItem(key, e) || placed.contains(key)) continue;
        append(items, newItems(key));
    }
    const Bytes ilst = makeBox("ilst", items);

    // ---- meta / udta / moov around it
    Bytes meta;
    if (p.meta) {
        Bytes content(p.moov.begin() + static_cast<std::ptrdiff_t>(p.meta->content),
                      p.moov.begin() + static_cast<std::ptrdiff_t>(p.metaContent));   // version / flags
        bool done = false;
        const auto kids = children(p.moov, p.metaContent, p.meta->end);
        for (const auto& c : kids) {
            if (c.is("ilst")) {
                if (!done) append(content, ilst);
                done = true;
                continue;
            }
            append(content, slice(p.moov, c));
            if (c.is("hdlr") && !done && !p.ilst) {
                append(content, ilst);
                done = true;
            }
        }
        if (!done) append(content, ilst);
        meta = makeBox("meta", content);
    } else {
        Bytes content{0, 0, 0, 0};
        Bytes hdlr{0, 0, 0, 0, 0, 0, 0, 0};   // version / flags, pre_defined
        append(hdlr, "mdirappl");
        hdlr.resize(hdlr.size() + 8, 0);       // reserved
        hdlr.push_back(0);                     // empty name
        append(content, makeBox("hdlr", hdlr));
        append(content, ilst);
        meta = makeBox("meta", content);
    }
    Bytes moovContent;
    if (p.metaInMoov) {
        for (const auto& c : p.moovKids) append(moovContent, c.is("meta") ? meta : slice(p.moov, c));
    } else if (p.udta) {
        Bytes udta;
        bool done = false;
        for (const auto& c : children(p.moov, p.udta->content, p.udta->end)) {
            if (c.is("meta")) {
                if (!done) append(udta, meta);
                done = true;
                continue;
            }
            append(udta, slice(p.moov, c));
        }
        if (!done) append(udta, meta);
        const Bytes newUdta = makeBox("udta", udta);
        for (const auto& c : p.moovKids) append(moovContent, c.is("udta") ? newUdta : slice(p.moov, c));
    } else {
        for (const auto& c : p.moovKids) append(moovContent, slice(p.moov, c));
        append(moovContent, makeBox("udta", meta));
    }
    Bytes moov = makeBox("moov", moovContent);

    // ---- placing it
    const Box& old = p.top[p.moovIndex];
    const int64_t delta = static_cast<int64_t>(moov.size()) - static_cast<int64_t>(old.end - old.start);
    Plan plan;
    plan.push_back(copyRange(0, old.start));
    const Box* after = p.moovIndex + 1 < p.top.size() ? &p.top[p.moovIndex + 1] : nullptr;
    if (delta == 0 || !after) {   // the same size, or nothing behind it that could move
        plan.push_back(literal(std::move(moov)));
        plan.push_back(copyRange(old.end, p.fileSize));
        return plan;
    }
    if (isFree(after->type) && after->end - after->start <= 0xFFFFFFFFull &&
        static_cast<int64_t>(after->end - after->start) - delta >= 8) {   // the free box behind it absorbs the change
        plan.push_back(literal(std::move(moov)));
        plan.push_back(literal(freeBox(static_cast<uint64_t>(static_cast<int64_t>(after->end - after->start) - delta))));
        plan.push_back(copyRange(after->end, p.fileSize));
        return plan;
    }
    if (delta <= -8) {   // shrank: a free box fills the gap
        plan.push_back(literal(std::move(moov)));
        plan.push_back(literal(freeBox(static_cast<uint64_t>(-delta))));
        plan.push_back(copyRange(old.end, p.fileSize));
        return plan;
    }
    // Everything behind moov moves by delta: so do the chunk offsets pointing there.
    for (const auto& o : chunkOffsets(moov)) {
        const uint64_t v = offsetValue(moov, o);
        if (v < old.end) continue;
        const uint64_t nv = static_cast<uint64_t>(static_cast<int64_t>(v) + delta);
        if (o.wide) writeBE64(&moov[o.at], nv);
        else if (nv > 0xFFFFFFFFull) fail(Status::TooLarge, "chunk offset overflow (stco)");
        else writeBE32(&moov[o.at], static_cast<uint32_t>(nv));
    }
    plan.push_back(literal(std::move(moov)));
    plan.push_back(copyRange(old.end, p.fileSize, fragmentPatches(s, p, old.end, delta)));
    return plan;
}

// Every chunk of the new file starts with the same bytes as in the original (a sample of up to 512 chunks).
void probeChunks(Source& orig, const Parsed& a, Source& made, const Parsed& b) {
    const auto oa = chunkOffsets(a.moov), ob = chunkOffsets(b.moov);
    if (oa.size() != ob.size()) fail(Status::VerifyFailed, "chunk tables differ");
    const size_t step = std::max<size_t>(1, oa.size() / 512);
    for (size_t i = 0; i < oa.size(); i += step) {
        const uint64_t x = offsetValue(a.moov, oa[i]), y = offsetValue(b.moov, ob[i]);
        if (x >= orig.size() || y >= made.size()) fail(Status::VerifyFailed, "chunk offset outside the file");
        const size_t n = static_cast<size_t>(std::min<uint64_t>({16, orig.size() - x, made.size() - y}));
        uint8_t p[16], q[16];
        if (!orig.read(x, p, n) || !made.read(y, q, n) || std::memcmp(p, q, n) != 0)
            fail(Status::VerifyFailed, "a chunk offset points at different data");
    }
}

} // namespace mp4

// =====================================================================================================================

using Model = std::variant<id3::Parsed, flac::Parsed, mp4::Parsed>;

Format sniff(Source& s, const fs::path& file) {
    uint8_t h[12] = {};
    const size_t n = static_cast<size_t>(std::min<uint64_t>(12, s.size()));
    if (n < 4 || !s.read(0, h, n)) return Format::Unknown;
    const std::string ext = extensionOf(file);
    if (std::memcmp(h, "ID3", 3) == 0 && n >= 10) {
        const uint64_t end = 10 + uint64_t(syncsafe(h + 6)) + ((h[5] & 0x10) ? 10 : 0);
        uint8_t m[8] = {};
        if (end + 8 <= s.size() && s.read(end, m, 8)) {
            if (std::memcmp(m, "fLaC", 4) == 0) return Format::Flac;
            if (std::memcmp(m + 4, "ftyp", 4) == 0) return Format::Unknown;   // an ID3 tag in front of MP4: not ours
        }
        return ext == ".mp3" ? Format::Mp3 : Format::Unknown;
    }
    if (std::memcmp(h, "fLaC", 4) == 0) return Format::Flac;
    if (n >= 8 && std::memcmp(h + 4, "ftyp", 4) == 0) return Format::Mp4;
    if (ext == ".mp3") {
        const bool adts = h[0] == 0xFF && (h[1] & 0xF6) == 0xF0;
        if (!adts) return Format::Mp3;
    }
    return Format::Unknown;
}

Model parseModel(Source& s, Format f) {
    switch (f) {
    case Format::Mp3: return id3::parse(s);
    case Format::Flac: return flac::parse(s);
    case Format::Mp4: return mp4::parse(s);
    default: fail(Status::Unsupported, "not a format whose tags can be written");
    }
}

// The format's functions for a parsed file.
struct Ops {
    static Tags tags(const Model& m) {
        if (const auto* p = std::get_if<id3::Parsed>(&m)) return id3::tagsOf(*p);
        if (const auto* p = std::get_if<flac::Parsed>(&m)) return flac::tagsOf(*p);
        return mp4::tagsOf(std::get<mp4::Parsed>(m));
    }
    static std::vector<Bytes> preserved(const Model& m, const Edit& e) {
        if (const auto* p = std::get_if<id3::Parsed>(&m)) return id3::preserved(*p, e);
        if (const auto* p = std::get_if<flac::Parsed>(&m)) return flac::preserved(*p, e);
        return mp4::preserved(std::get<mp4::Parsed>(m), e);
    }
    static Ranges payload(const Model& m) {
        if (const auto* p = std::get_if<id3::Parsed>(&m)) return id3::payload(*p);
        if (const auto* p = std::get_if<flac::Parsed>(&m)) return flac::payload(*p);
        return mp4::payload(std::get<mp4::Parsed>(m));
    }
    static Plan plan(Source& s, const Model& m, const Edit& e, const Tags& current) {
        if (const auto* p = std::get_if<id3::Parsed>(&m)) return id3::plan(*p, e, current);
        if (const auto* p = std::get_if<flac::Parsed>(&m)) return flac::plan(*p, e, current);
        return mp4::plan(s, std::get<mp4::Parsed>(m), e, current);
    }
};

Edit normalized(const Edit& in) {
    Edit e = in;
    for (auto* s : {&e.title, &e.album, &e.albumArtist, &e.genre})
        if (*s) **s = trim(**s);
    if (e.artist) *e.artist = normalizeList(*e.artist);
    for (auto* n : {&e.track, &e.disc, &e.year})
        if (*n) **n = std::max(0, **n);
    if (e.cover == Edit::Cover::Set) {
        if (e.picture.data.empty()) fail(Status::IoError, "no picture to embed");
        e.picture.type = 3;
        if (e.picture.mime.empty()) e.picture.mime = mimeOf(e.picture.data);
    }
    return e;
}

// Which field differs (log text for a failed check).
std::string firstDifference(const Tags& a, const Tags& b) {
    if (a.title != b.title) return "title";
    if (a.artist != b.artist) return "artist";
    if (a.album != b.album) return "album";
    if (a.albumArtist != b.albumArtist) return "album artist";
    if (a.genre != b.genre) return "genre";
    if (a.track != b.track || a.trackTotal != b.trackTotal) return "track";
    if (a.disc != b.disc || a.discTotal != b.discTotal) return "disc";
    if (a.year != b.year) return "year";
    if (a.cover.has_value() != b.cover.has_value() || (a.cover && a.cover->data != b.cover->data)) return "cover";
    return "?";
}

std::wstring randomTag() {
    std::random_device rd;
    wchar_t b[17];
    swprintf(b, 17, L"%08x%08x", rd(), rd());
    return b;
}

// Writes the plan to `temp`; returns the SHA-256 of the original's payload ranges (hashed while they are copied).
std::string writePlan(Source& src, const Plan& plan, const Ranges& payload, const fs::path& temp) {
    const HANDLE out = CreateFileW(winPath(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        fail(statusForError(err), "can't create the temporary file (error " + std::to_string(err) + ")");
    }
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{out};
    const auto put = [&](const uint8_t* p, size_t n) {
        while (n > 0) {
            DWORD wrote = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
            if (!WriteFile(out, p, chunk, &wrote, nullptr) || wrote == 0) {
                const DWORD err = GetLastError();
                fail(err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL ? Status::IoError : statusForError(err),
                     "writing the temporary file failed (error " + std::to_string(err) + ")");
            }
            p += wrote;
            n -= wrote;
        }
    };
    Sha256 hash;
    uint64_t hashed = 0, expected = 0;
    for (const auto& [a, b] : payload) expected += b - a;
    size_t range = 0;
    Bytes buf(1u << 20);
    for (const auto& piece : plan) {
        if (!piece.copy) {
            put(piece.bytes.data(), piece.bytes.size());
            continue;
        }
        for (uint64_t pos = piece.from; pos < piece.to;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), piece.to - pos));
            if (!src.read(pos, buf.data(), n)) fail(Status::IoError, "reading the original failed");
            // Hash the payload parts of this chunk (before any patch: patches never touch the payload).
            while (range < payload.size() && payload[range].second <= pos) ++range;
            for (size_t r = range; r < payload.size() && payload[r].first < pos + n; ++r) {
                const uint64_t from = std::max(pos, payload[r].first), to = std::min(pos + n, payload[r].second);
                if (from < to) {
                    hash.add(buf.data() + (from - pos), static_cast<size_t>(to - from));
                    hashed += to - from;
                }
            }
            for (const auto& patch : piece.patches) {
                if (patch.at + patch.bytes.size() <= pos || patch.at >= pos + n) continue;
                for (size_t k = 0; k < patch.bytes.size(); ++k) {
                    const uint64_t at = patch.at + k;
                    if (at < pos || at >= pos + n) continue;
                    for (const auto& [pa, pb] : payload)
                        if (at >= pa && at < pb) fail(Status::Corrupt, "an offset patch falls into the audio payload");
                    buf[static_cast<size_t>(at - pos)] = patch.bytes[k];
                }
            }
            put(buf.data(), n);
            pos += n;
        }
    }
    if (hashed != expected) fail(Status::Corrupt, "the audio payload isn't copied as a whole");
    if (!FlushFileBuffers(out)) fail(Status::IoError, "flushing the temporary file failed");
    return hash.hex();
}

void verifyTemp(Source& orig, const Model& before, Format format, const fs::path& temp, const Edit& e, const Tags& expected,
                const std::vector<Bytes>& keep, const std::string& payloadHash) {
    Source t(temp);
    if (!t.ok()) fail(Status::IoError, "can't reopen the temporary file");
    Model after;
    try {
        after = parseModel(t, format);
    } catch (const Fail& f) {
        fail(Status::VerifyFailed, "the new file doesn't parse: " + f.detail);
    }
    const Tags got = Ops::tags(after);
    if (got != expected) fail(Status::VerifyFailed, "the new file's " + firstDifference(got, expected) + " isn't what was written");
    if (Ops::preserved(after, e) != keep) fail(Status::VerifyFailed, "frames that weren't edited changed");
    if (hashRanges(t, Ops::payload(after)) != payloadHash) fail(Status::VerifyFailed, "the audio payload differs");
    if (format == Format::Mp4) mp4::probeChunks(orig, std::get<mp4::Parsed>(before), t, std::get<mp4::Parsed>(after));
}

} // namespace

// =====================================================================================================================
// Public API

const char* formatName(Format f) {
    switch (f) {
    case Format::Mp3: return "MP3";
    case Format::Flac: return "FLAC";
    case Format::Mp4: return "MP4";
    default: return "";
    }
}

bool writableExtension(const fs::path& file) {
    const std::string ext = extensionOf(file);
    return ext == ".mp3" || ext == ".flac" || ext == ".m4a" || ext == ".mp4" || ext == ".m4b";
}

bool Edit::empty() const {
    return !title && !artist && !album && !albumArtist && !genre && !track && !disc && !year && cover == Cover::Keep;
}

std::string normalizeList(const std::string& s) {
    std::string out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t semi = s.find(';', start);
        const std::string part = trim(std::string_view(s).substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        if (!part.empty()) out += (out.empty() ? "" : "; ") + part;
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return out;
}

Tags applied(Tags t, const Edit& e) {
    if (e.title) t.title = trim(*e.title);
    if (e.artist) t.artist = normalizeList(*e.artist);
    if (e.album) t.album = trim(*e.album);
    if (e.albumArtist) t.albumArtist = trim(*e.albumArtist);
    if (e.genre) t.genre = trim(*e.genre);
    if (e.track) {
        t.track = std::max(0, *e.track);
        if (t.track == 0) t.trackTotal = 0;
    }
    if (e.disc) {
        t.disc = std::max(0, *e.disc);
        if (t.disc == 0) t.discTotal = 0;
    }
    if (e.year) t.year = std::max(0, *e.year);
    if (e.cover == Edit::Cover::Remove) t.cover.reset();
    else if (e.cover == Edit::Cover::Set) {
        Picture p = e.picture;
        p.type = 3;
        if (p.mime.empty()) p.mime = mimeOf(p.data);
        t.cover = std::move(p);
    }
    return t;
}

const char* toString(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::Unsupported: return "unsupported";
    case Status::NotFound: return "not found";
    case Status::ReadOnly: return "read-only";
    case Status::InUse: return "in use";
    case Status::Corrupt: return "corrupt";
    case Status::TooLarge: return "too large";
    case Status::IoError: return "i/o error";
    case Status::VerifyFailed: return "verify failed";
    case Status::Changed: return "changed meanwhile";
    }
    return "?";
}

ReadResult read(const fs::path& file) {
    ReadResult r;
    try {
        Source s(file);
        if (!s.ok()) fail(statusForError(s.error()), "can't open the file (error " + std::to_string(s.error()) + ")");
        r.format = sniff(s, file);
        if (r.format == Format::Unknown) fail(Status::Unsupported, "not a format whose tags can be written");
        r.tags = Ops::tags(parseModel(s, r.format));
        r.ok = true;
    } catch (const Fail& f) {
        r.error = std::string(toString(f.status)) + ": " + f.detail;
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    return r;
}

WriteResult write(const fs::path& file, const Edit& edit, const WriteOptions& options) {
    WriteResult result;
    fs::path temp, backup;
    const auto cleanup = [&] {
        if (!temp.empty()) DeleteFileW(winPath(temp).c_str());
    };
    try {
        if (!writableExtension(file)) fail(Status::Unsupported, "not a format whose tags can be written");
        const Edit e = normalized(edit);
        std::unique_ptr<Source> src = std::make_unique<Source>(file);
        if (!src->ok()) fail(statusForError(src->error()), "can't open the file (error " + std::to_string(src->error()) + ")");
        if (src->attributes() & FILE_ATTRIBUTE_READONLY) fail(Status::ReadOnly, "the file is read-only");
        const Format format = sniff(*src, file);
        if (format == Format::Unknown) fail(Status::Unsupported, "the content is not MP3 / FLAC / MP4");
        const Model model = parseModel(*src, format);
        const Tags current = Ops::tags(model);
        const Tags expected = applied(current, e);
        if (e.empty() || expected == current) {   // nothing changes: the file stays as it is
            result.status = Status::Ok;
            result.size = static_cast<int64_t>(src->size());
            result.mtime = src->mtime();
            return result;
        }
        const Plan plan = Ops::plan(*src, model, e, current);
        const std::vector<Bytes> keep = Ops::preserved(model, e);
        const Ranges payload = Ops::payload(model);

        const std::wstring tag = randomTag();
        const std::wstring name = file.filename().wstring();
        temp = file.parent_path() / (L"~" + name + L"." + tag + L".shadetube.tmp");
        backup = file.parent_path() / (L"~" + name + L"." + tag + L".shadetube.bak");
        const std::string hash = writePlan(*src, plan, payload, temp);
        if (options.tamper) options.tamper(temp);
        if (options.verify) verifyTemp(*src, model, format, temp, e, expected, keep, hash);

        // The original must be the one that was read (another tagger may have written it meanwhile).
        const uint64_t size0 = src->size(), mtime0 = src->mtime();
        const DWORD attrs0 = src->attributes();
        src.reset();
        {
            Source again(file);
            if (!again.ok()) fail(statusForError(again.error()), "the file went away");
            if (again.size() != size0 || again.mtime() != mtime0) fail(Status::Changed, "the file changed while it was being written");
        }

        // Swap. With a backup name ReplaceFileW never leaves the original missing: on ERROR_UNABLE_TO_MOVE_REPLACEMENT
        // both files keep their names, on ..._2 the original sits under the backup name (moved back below).
        DWORD err = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (ReplaceFileW(winPath(file).c_str(), winPath(temp).c_str(), winPath(backup).c_str(),
                             REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
                err = 0;
                break;
            }
            err = GetLastError();
            if (err == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                if (!MoveFileExW(winPath(backup).c_str(), winPath(file).c_str(), MOVEFILE_REPLACE_EXISTING))
                    ST_LOG_ERROR("tags", "could not move the original back from {} (error {})", toUtf8(backup.wstring()), GetLastError());
                break;
            }
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION && err != ERROR_ACCESS_DENIED &&
                err != ERROR_UNABLE_TO_REMOVE_REPLACED)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));   // a scanner / indexer let go
        }
        if (err != 0) {
            const Status st = err == ERROR_UNABLE_TO_REMOVE_REPLACED ? Status::InUse
                              : err == ERROR_UNABLE_TO_MOVE_REPLACEMENT || err == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2
                                  ? Status::IoError
                                  : statusForError(err);
            fail(st, "ReplaceFileW failed (error " + std::to_string(err) + ")");
        }
        temp.clear();   // it is the file now
        // The original, now under the backup name (the playing file's mapping may still use it: deleting just
        // removes the name).
        bool gone = false;
        for (int attempt = 0; attempt < 5 && !gone; ++attempt) {
            gone = DeleteFileW(winPath(backup).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
            if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!gone) ST_LOG_WARN("tags", "could not delete the backup {}", toUtf8(backup.wstring()));
        // The new file takes the original's attributes (it was created hidden).
        const DWORD now = GetFileAttributesW(winPath(file).c_str());
        const DWORD want = attrs0 & ~(FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NORMAL);
        if (now != INVALID_FILE_ATTRIBUTES && (now & ~FILE_ATTRIBUTE_ARCHIVE) != (want & ~FILE_ATTRIBUTE_ARCHIVE))
            SetFileAttributesW(winPath(file).c_str(), want ? want : FILE_ATTRIBUTE_NORMAL);
        Source done(file);
        result.status = Status::Ok;
        result.size = static_cast<int64_t>(done.size());
        result.mtime = done.mtime();
        ST_LOG_INFO("tags", "{} tags written ({} -> {} bytes)", formatName(format), size0, done.size());
    } catch (const Fail& f) {
        cleanup();
        result.status = f.status;
        result.detail = f.detail;
        ST_LOG_WARN("tags", "tags not written ({}): {}", toString(f.status), f.detail);
    } catch (const std::exception& ex) {
        cleanup();
        result.status = Status::IoError;
        result.detail = ex.what();
        ST_LOG_WARN("tags", "tags not written: {}", ex.what());
    }
    return result;
}

std::string payloadHash(const fs::path& file) {
    try {
        Source s(file);
        if (!s.ok()) return {};
        const Format f = sniff(s, file);
        if (f == Format::Unknown) return {};
        return hashRanges(s, Ops::payload(parseModel(s, f)));
    } catch (...) {
        return {};
    }
}

std::pair<int, int> imageSize(const std::vector<uint8_t>& d) {
    if (d.size() >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return {static_cast<int>(be32(&d[16])), static_cast<int>(be32(&d[20]))};
    if (d.size() >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < d.size()) {
            if (d[i] != 0xFF) {
                ++i;
                continue;
            }
            const uint8_t m = d[i + 1];
            if (m == 0xFF) {
                ++i;
                continue;
            }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                i += 2;
                continue;
            }
            const size_t len = be16(&d[i + 2]);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
                return {static_cast<int>(be16(&d[i + 7])), static_cast<int>(be16(&d[i + 5]))};
            i += 2 + len;
        }
    }
    return {0, 0};
}

std::string id3v1Genre(int n) {
    return n >= 0 && n < static_cast<int>(std::size(kGenres)) ? kGenres[n] : std::string();
}

int id3v1GenreIndex(const std::string& name) {
    const std::string l = lowerAscii(trim(name));
    for (int i = 0; i < static_cast<int>(std::size(kGenres)); ++i)
        if (lowerAscii(kGenres[i]) == l) return i;
    return -1;
}

} // namespace st::audio::tags
