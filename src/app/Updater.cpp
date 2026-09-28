// Self-update from GitHub releases: version compare, release JSON, ranged download, a minimal ZIP reader (stored +
// deflate, CRC-32), exe verification and the rename swap. See Updater.h.
#include "app/Updater.h"

#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <utility>

#pragma comment(lib, "version.lib")   // GetFileVersionInfoW / VerQueryValueW
#pragma comment(lib, "bcrypt.lib")    // SHA-256

namespace st::app::updater {

namespace fs = std::filesystem;

namespace {

constexpr char kLatestUrl[] = "https://api.github.com/repos/shadesofdeath/ShadeTube/releases/latest";

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string env(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? toUtf8(std::wstring_view(buf, n)) : std::string{};
}

// "Erişim engellendi" (the system message in the user's Windows language) for a Win32 error code.
std::wstring winError(DWORD code) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring text = n && msg ? std::wstring(msg, n) : i18n::format(tr(L"hata {}"), static_cast<long long>(code));
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
    return text;
}

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
// User-facing failures: the text in the UI language, thrown as UTF-8 (toasted as is).
[[noreturn]] void fail(std::wstring_view message) { throw std::runtime_error(toUtf8(message)); }

// ---- VERSIONINFO -----------------------------------------------------------------------------------------
std::string versionString(const fs::path& exe, const wchar_t* key) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    std::vector<std::wstring> blocks;
    struct LangCodePage {
        WORD lang, codePage;
    };
    LangCodePage* langs = nullptr;
    UINT langBytes = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &langBytes) && langs)
        for (UINT i = 0; i < langBytes / sizeof(LangCodePage); ++i)
            blocks.push_back(std::format(L"{:04x}{:04x}", langs[i].lang, langs[i].codePage));
    blocks.push_back(L"040904b0");
    blocks.push_back(L"040904e4");
    for (const auto& b : blocks) {
        const std::wstring query = L"\\StringFileInfo\\" + b + L"\\" + key;
        wchar_t* value = nullptr;
        UINT len = 0;
        if (VerQueryValueW(data.data(), query.c_str(), reinterpret_cast<void**>(&value), &len) && value && len > 0)
            return toUtf8(std::wstring_view(value, wcsnlen(value, len)));
    }
    return {};
}

// ---- Inflate (RFC 1951), after Mark Adler's puff.c: small, strict, fast enough for a few MB --------------
constexpr int kMaxBits = 15, kMaxLCodes = 286, kMaxDCodes = 30, kFixLCodes = 288;

struct Huffman {
    short count[kMaxBits + 1];   // number of codes of each length
    short symbol[kFixLCodes];    // symbols ordered by code
};

// Builds a canonical Huffman table from code lengths. Returns 0 for a complete code, > 0 for an incomplete one and
// < 0 for an over-subscribed (invalid) one.
int construct(Huffman& h, const short* length, int n) {
    for (short& c : h.count) c = 0;
    for (int s = 0; s < n; ++s) ++h.count[length[s]];
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[kMaxBits + 1]{};
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<short>(s);
    return left;
}

class Inflater {
public:
    Inflater(std::string_view in, std::string& out, size_t limit) : in_(in), out_(out), limit_(limit) {}

    void run() {
        int last = 0;
        do {
            last = bits(1);
            switch (bits(2)) {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: bad();
            }
        } while (!last);
    }

private:
    [[noreturn]] static void bad() { fail(tr(L"Paket bozuk (deflate verisi okunamadı).")); }

    int bits(int need) {
        uint32_t val = bitBuf_;
        while (bitCnt_ < need) {
            if (pos_ >= in_.size()) bad();
            val |= static_cast<uint32_t>(static_cast<uint8_t>(in_[pos_++])) << bitCnt_;
            bitCnt_ += 8;
        }
        bitBuf_ = val >> need;
        bitCnt_ -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    void stored() {
        bitBuf_ = 0;   // to a byte boundary
        bitCnt_ = 0;
        if (pos_ + 4 > in_.size()) bad();
        auto byte = [&](size_t i) { return static_cast<unsigned>(static_cast<uint8_t>(in_[i])); };
        const unsigned len = byte(pos_) | byte(pos_ + 1) << 8;
        const unsigned nlen = byte(pos_ + 2) | byte(pos_ + 3) << 8;
        pos_ += 4;
        if (len != (~nlen & 0xffffu) || pos_ + len > in_.size() || out_.size() + len > limit_) bad();
        out_.append(in_.data() + pos_, len);
        pos_ += len;
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        bad();
    }

    void codes(const Huffman& lencode, const Huffman& distcode) {
        static constexpr short kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr short kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
                                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 256) {
                if (out_.size() >= limit_) bad();
                out_.push_back(static_cast<char>(symbol));
            } else if (symbol == 256) {
                return;
            } else {
                symbol -= 257;
                if (symbol >= 29) bad();
                const size_t len = static_cast<size_t>(kLenBase[symbol] + bits(kLenExtra[symbol]));
                const int dsym = decode(distcode);
                if (dsym < 0 || dsym >= 30) bad();
                const size_t dist = static_cast<size_t>(kDistBase[dsym] + bits(kDistExtra[dsym]));
                if (dist > out_.size() || out_.size() + len > limit_) bad();
                const size_t from = out_.size() - dist;
                for (size_t i = 0; i < len; ++i) {
                    const char c = out_[from + i];   // may overlap the bytes being written
                    out_.push_back(c);
                }
            }
        }
    }

    void fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t{};
            short lengths[kFixLCodes];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < kFixLCodes; ++s) lengths[s] = 8;
            construct(t.first, lengths, kFixLCodes);
            for (s = 0; s < kMaxDCodes; ++s) lengths[s] = 5;
            construct(t.second, lengths, kMaxDCodes);
            return t;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        static constexpr short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > kMaxLCodes || ndist > kMaxDCodes) bad();
        short lengths[kMaxLCodes + kMaxDCodes]{};
        int index = 0;
        for (; index < ncode; ++index) lengths[kOrder[index]] = static_cast<short>(bits(3));
        for (; index < 19; ++index) lengths[kOrder[index]] = 0;
        Huffman lencode{}, distcode{};
        if (construct(lencode, lengths, 19) != 0) bad();   // the code-length code must be complete
        index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 16) {
                lengths[index++] = static_cast<short>(symbol);
                continue;
            }
            short len = 0;
            if (symbol == 16) {
                if (index == 0) bad();
                len = lengths[index - 1];
                symbol = 3 + bits(2);
            } else if (symbol == 17) {
                symbol = 3 + bits(3);
            } else {
                symbol = 11 + bits(7);
            }
            if (index + symbol > nlen + ndist) bad();
            while (symbol--) lengths[index++] = len;
        }
        if (lengths[256] == 0) bad();   // no end-of-block code
        // An incomplete code is only allowed when it has a single code (RFC 1951 / zlib behaviour).
        int err = construct(lencode, lengths, nlen);
        if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) bad();
        err = construct(distcode, lengths + nlen, ndist);
        if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) bad();
        codes(lencode, distcode);
    }

    std::string_view in_;
    std::string& out_;
    size_t limit_;
    size_t pos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCnt_ = 0;
};

uint32_t rd16(const std::string& s, size_t o) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[o])) | static_cast<uint32_t>(static_cast<uint8_t>(s[o + 1])) << 8;
}
uint32_t rd32(const std::string& s, size_t o) { return rd16(s, o) | rd16(s, o + 2) << 16; }

std::string readFile(const fs::path& file, uintmax_t maxBytes) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) fail(i18n::format(tr(L"{} okunamadı."), {file.filename().wstring()}));
    if (size > maxBytes) fail(tr(L"Paket beklenenden çok büyük."));
    std::ifstream f(file, std::ios::binary);
    std::string data(static_cast<size_t>(size), '\0');
    if (!f.read(data.data(), static_cast<std::streamsize>(size))) fail(i18n::format(tr(L"{} okunamadı."), {file.filename().wstring()}));
    return data;
}

void writeFile(const fs::path& file, std::string_view data) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (f.fail()) fail(i18n::format(tr(L"{} yazılamadı."), {file.filename().wstring()}));
}

// MoveFileEx with a few retries while an antivirus scanner or the exiting process briefly holds the file.
bool moveRetry(const fs::path& from, const fs::path& to) {
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD err = GetLastError();
        if ((err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION) || attempt >= 9) {
            SetLastError(err);
            return false;
        }
        Sleep(200);
    }
}

bool iequalsW(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

Cancelled::Cancelled() : std::runtime_error(toUtf8(tr(L"Güncelleme iptal edildi"))) {}   // the same text as the UI's toast

// ---- Versions ---------------------------------------------------------------------------------------------

std::optional<Version> parseVersion(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    if (const auto plus = s.find('+'); plus != std::string_view::npos) s = s.substr(0, plus);   // build metadata
    Version v;
    if (const auto dash = s.find('-'); dash != std::string_view::npos) {
        v.pre = std::string(s.substr(dash + 1));
        s = s.substr(0, dash);
        if (v.pre.empty()) return std::nullopt;
    }
    int parts[3] = {0, 0, 0};
    int n = 0;
    while (!s.empty()) {
        if (n == 3) return std::nullopt;
        const auto dot = s.find('.');
        const std::string_view part = s.substr(0, dot);
        if (part.empty() || part.size() > 9) return std::nullopt;
        const auto [p, ec] = std::from_chars(part.data(), part.data() + part.size(), parts[n]);
        if (ec != std::errc{} || p != part.data() + part.size()) return std::nullopt;
        ++n;
        if (dot == std::string_view::npos) break;
        s.remove_prefix(dot + 1);
        if (s.empty()) return std::nullopt;   // "1.2."
    }
    if (n == 0) return std::nullopt;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    return v;
}

int compareVersions(std::string_view a, std::string_view b) {
    const Version va = parseVersion(a).value_or(Version{}), vb = parseVersion(b).value_or(Version{});
    for (const auto& [x, y] : {std::pair{va.major, vb.major}, std::pair{va.minor, vb.minor}, std::pair{va.patch, vb.patch}})
        if (x != y) return x < y ? -1 : 1;
    // A pre-release precedes the release; otherwise compare the dot-separated identifiers (semver §11).
    if (va.pre.empty() || vb.pre.empty()) return va.pre.empty() == vb.pre.empty() ? 0 : va.pre.empty() ? 1 : -1;
    std::string_view pa = va.pre, pb = vb.pre;
    for (;;) {
        const auto da = pa.find('.'), db = pb.find('.');
        const std::string_view ia = pa.substr(0, da), ib = pb.substr(0, db);
        const bool na = !ia.empty() && std::all_of(ia.begin(), ia.end(), [](char c) { return c >= '0' && c <= '9'; });
        const bool nb = !ib.empty() && std::all_of(ib.begin(), ib.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (na && nb) {
            if (ia.size() != ib.size()) return ia.size() < ib.size() ? -1 : 1;   // numeric: longer = larger
            if (ia != ib) return ia < ib ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1;   // numeric identifiers have lower precedence
        } else if (ia != ib) {
            return ia < ib ? -1 : 1;
        }
        const bool ea = da == std::string_view::npos, eb = db == std::string_view::npos;
        if (ea || eb) return ea == eb ? 0 : ea ? -1 : 1;   // fewer identifiers = lower precedence
        pa.remove_prefix(da + 1);
        pb.remove_prefix(db + 1);
    }
}

std::string normalizeVersion(std::string_view tag) {
    if (const auto v = parseVersion(tag)) return std::format("{}.{}.{}{}{}", v->major, v->minor, v->patch, v->pre.empty() ? "" : "-", v->pre);
    std::string t(tag);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\n')) t.pop_back();
    t.erase(0, t.find_first_not_of(' '));
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t.erase(0, 1);
    return t;
}

std::string fileProductVersion(const fs::path& exe) { return versionString(exe, L"ProductVersion"); }
std::string fileProductName(const fs::path& exe) { return versionString(exe, L"ProductName"); }

fs::path currentExe() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

const std::string& currentVersion() {
    static const std::string version = [] {
        if (std::string o = env(L"SHADETUBE_VERSION_OVERRIDE"); !o.empty()) return normalizeVersion(o);
        const std::string v = fileProductVersion(currentExe());
        return v.empty() ? std::string("0.0.0") : normalizeVersion(v);
    }();
    return version;
}

// ---- Releases ---------------------------------------------------------------------------------------------

std::vector<std::string> findSha256(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        if (lower(line).find("sha256") != std::string::npos) {
            for (size_t i = 0; i < line.size();) {
                if (!isHex(line[i])) {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < line.size() && isHex(line[j])) ++j;
                if (j - i == 64) out.push_back(lower(line.substr(i, 64)));
                i = j;
            }
        }
        start = end + 1;
    }
    return out;
}

Release parseRelease(std::string_view json) {
    const auto j = nlohmann::json::parse(json);
    auto str = [](const nlohmann::json& o, const char* key) {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    Release r;
    r.tag = str(j, "tag_name");
    if (r.tag.empty()) fail("release: tag_name missing");
    r.version = normalizeVersion(r.tag);
    r.name = str(j, "name");
    r.body = str(j, "body");
    r.htmlUrl = str(j, "html_url");
    r.publishedAt = str(j, "published_at");
    if (const auto it = j.find("assets"); it != j.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (!a.is_object()) continue;
            Asset asset;
            asset.name = str(a, "name");
            asset.url = str(a, "browser_download_url");
            if (const auto s = a.find("size"); s != a.end() && s->is_number_integer()) asset.size = s->get<int64_t>();
            // GitHub computes "digest": "sha256:<hex>" for every uploaded asset.
            if (const std::string d = lower(str(a, "digest")); d.size() == 7 + 64 && d.rfind("sha256:", 0) == 0 &&
                                                               std::all_of(d.begin() + 7, d.end(), isHex))
                asset.sha256 = d.substr(7);
            if (!asset.name.empty() && !asset.url.empty()) r.assets.push_back(std::move(asset));
        }
    }
    r.notesSha256 = findSha256(r.body);
    return r;
}

const Asset* pickAsset(const Release& r) {
    const std::string exact = "shadetube-" + lower(r.version) + "-win64.zip";
    const Asset *zip = nullptr, *exe = nullptr;
    for (const auto& a : r.assets) {
        const std::string n = lower(a.name);
        if (n == exact) return &a;
        if (n.find("shadetube") == std::string::npos || n.find("arm64") != std::string::npos) continue;
        if (endsWith(n, ".zip") && (!zip || (n.find("win64") != std::string::npos && lower(zip->name).find("win64") == std::string::npos)))
            zip = &a;
        else if (endsWith(n, ".exe") && !exe)
            exe = &a;
    }
    return zip ? zip : exe;
}

std::string updateUrl() {
    std::string url = env(L"SHADETUBE_UPDATE_URL");
    return url.empty() ? std::string(kLatestUrl) : url;
}

std::string userAgent() { return "ShadeTube/" + currentVersion() + " (+https://github.com/shadesofdeath/ShadeTube)"; }

CheckResult check(std::string_view runningVersion) {
    const std::string url = updateUrl();
    http::HttpRequest req;
    req.url = url;
    req.headers = {{"User-Agent", userAgent()}, {"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    http::HttpResponse resp;
    try {
        resp = http::send(std::move(req));
    } catch (const std::exception& e) {
        ST_LOG_WARN("update", "check {}: {}", url, e.what());
        fail(tr(L"GitHub'a ulaşılamadı. İnternet bağlantını kontrol et."));
    }
    if (resp.statusCode == 404) {   // the repository has no (public) release yet
        ST_LOG_INFO("update", "no published release ({})", url);
        return {CheckStatus::NoReleases, {}};
    }
    if (resp.statusCode == 403 || resp.statusCode == 429) fail(tr(L"GitHub istek sınırına ulaşıldı. Biraz sonra tekrar dene."));
    if (!resp.isSuccessStatusCode()) fail(i18n::format(tr(L"GitHub beklenmeyen bir yanıt verdi (HTTP {})."), resp.statusCode));
    CheckResult result;
    try {
        result.release = parseRelease(resp.body);
    } catch (const std::exception& e) {
        ST_LOG_WARN("update", "release JSON: {}", e.what());
        fail(tr(L"Sürüm bilgisi okunamadı."));
    }
    result.status = compareVersions(result.release.version, runningVersion) > 0 ? CheckStatus::Available : CheckStatus::UpToDate;
    ST_LOG_INFO("update", "latest release {} (running {}): {}", result.release.version, runningVersion,
                result.status == CheckStatus::Available ? "update available" : "up to date");
    return result;
}

// ---- Download / verify / swap -------------------------------------------------------------------------------

void download(const std::string& url, const fs::path& out, const ProgressFn& progress, const CancelToken& cancel) {
    constexpr int64_t kChunk = 1 << 20;
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file) fail(i18n::format(tr(L"{} klasörüne yazılamıyor."), {out.parent_path().wstring()}));
    int64_t done = 0, total = -1;
    for (int request = 0;; ++request) {
        if (cancel.isCancellationRequested()) throw Cancelled();
        if (request > 100000) fail(tr(L"İndirme tamamlanamadı."));
        http::HttpRequest req;
        req.url = url;
        req.headers = {{"User-Agent", userAgent()},
                       {"Accept", "application/octet-stream"},
                       {"Accept-Encoding", "identity"},   // byte ranges of the file itself, never of a gzip stream
                       {"Range", std::format("bytes={}-{}", done, done + kChunk - 1)}};
        http::HttpResponse resp;
        try {
            resp = http::send(std::move(req), cancel);   // the token also aborts a chunk mid-read (every 64 KB)
        } catch (const std::exception& e) {
            if (cancel.isCancellationRequested()) throw Cancelled();
            ST_LOG_WARN("update", "download {} at {}: {}", url, done, e.what());
            fail(tr(L"İndirme bağlantısı koptu. İnternet bağlantını kontrol et."));
        }
        if (resp.statusCode == 206) {
            // Content-Range: bytes <first>-<last>/<total or *>
            const std::string range = resp.header("Content-Range").value_or("");
            int64_t first = -1;
            if (const auto sp = range.find(' '); sp != std::string::npos) {
                const auto dash = range.find('-', sp);
                std::from_chars(range.data() + sp + 1, range.data() + (dash == std::string::npos ? range.size() : dash), first);
            }
            if (const auto slash = range.find('/'); slash != std::string::npos && range.compare(slash + 1, 1, "*") != 0)
                std::from_chars(range.data() + slash + 1, range.data() + range.size(), total);
            if (first != done) fail(tr(L"Sunucu beklenmeyen bir parça gönderdi."));
            file.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
            done += static_cast<int64_t>(resp.body.size());
        } else if (resp.statusCode == 200) {
            // The server ignores Range: this response is the whole file.
            if (done > 0) {
                file.close();
                file.open(out, std::ios::binary | std::ios::trunc);
            }
            file.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
            done = total = static_cast<int64_t>(resp.body.size());
        } else if (resp.statusCode == 416 && total >= 0 && done >= total) {
            break;
        } else {
            fail(i18n::format(tr(L"İndirme başarısız (HTTP {})."), resp.statusCode));
        }
        if (!file) fail(tr(L"İndirilen dosya diske yazılamadı."));
        if (progress) progress(done, total);
        if (total >= 0 ? done >= total : static_cast<int64_t>(resp.body.size()) < kChunk) break;
        if (resp.body.empty()) fail(tr(L"Sunucu boş yanıt verdi."));
    }
    file.close();
    if (file.fail()) fail(tr(L"İndirilen dosya diske yazılamadı."));
}

std::string sha256File(const fs::path& file) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) fail(tr(L"SHA-256 kullanılamıyor."));
    struct Close {
        BCRYPT_ALG_HANDLE& a;
        BCRYPT_HASH_HANDLE& h;
        ~Close() {
            if (h) BCryptDestroyHash(h);
            if (a) BCryptCloseAlgorithmProvider(a, 0);
        }
    } close{alg, hash};
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) fail(tr(L"SHA-256 kullanılamıyor."));
    std::ifstream f(file, std::ios::binary);
    if (!f) fail(i18n::format(tr(L"{} okunamadı."), {file.filename().wstring()}));
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (const auto n = f.gcount(); n > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
    }
    UCHAR digest[32];
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0))) fail(tr(L"SHA-256 hesaplanamadı."));
    std::string hex;
    for (UCHAR b : digest) hex += std::format("{:02x}", b);
    return hex;
}

uint32_t crc32(const void* data, size_t size, uint32_t crc) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::string inflate(std::string_view compressed, size_t expectedSize) {
    std::string out;
    out.reserve(expectedSize);
    Inflater(compressed, out, expectedSize).run();
    if (out.size() != expectedSize) fail(tr(L"Paket bozuk (beklenen boyut tutmuyor)."));
    return out;
}

void extractZipEntry(const fs::path& zip, std::string_view fileName, const fs::path& out) {
    const std::string data = readFile(zip, 512ull << 20);
    // End of central directory: the last "PK\5\6" within the trailing 64 KB comment range.
    if (data.size() < 22) fail(tr(L"Paket bozuk (ZIP değil)."));
    size_t eocd = std::string::npos;
    for (size_t i = data.size() - 22;; --i) {
        if (rd32(data, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || data.size() - i > 22 + 0xffff) break;
    }
    if (eocd == std::string::npos) fail(tr(L"Paket bozuk (ZIP dizini bulunamadı)."));
    const size_t entries = rd16(data, eocd + 10), cdSize = rd32(data, eocd + 12), cdOffset = rd32(data, eocd + 16);
    if (entries == 0xffff || cdOffset == 0xffffffff) fail(tr(L"ZIP64 paketleri desteklenmiyor."));
    if (cdOffset + cdSize > eocd) fail(tr(L"Paket bozuk (ZIP dizini)."));

    struct Entry {
        size_t local = 0, compressed = 0, size = 0;
        uint32_t crc = 0, method = 0, flags = 0;
        size_t depth = 0;
    };
    std::optional<Entry> best;
    const std::wstring wanted = toWide(fileName);
    size_t p = cdOffset;
    for (size_t i = 0; i < entries; ++i) {
        if (p + 46 > eocd || rd32(data, p) != 0x02014b50) fail(tr(L"Paket bozuk (ZIP girdisi)."));
        Entry e;
        e.flags = rd16(data, p + 8);
        e.method = rd16(data, p + 10);
        e.crc = rd32(data, p + 16);
        e.compressed = rd32(data, p + 20);
        e.size = rd32(data, p + 24);
        const size_t nameLen = rd16(data, p + 28), extraLen = rd16(data, p + 30), commentLen = rd16(data, p + 32);
        e.local = rd32(data, p + 42);
        if (p + 46 + nameLen > eocd) fail(tr(L"Paket bozuk (ZIP girdisi)."));
        std::string name = data.substr(p + 46, nameLen);
        std::replace(name.begin(), name.end(), '\\', '/');   // Windows PowerShell 5.1's Compress-Archive writes '\'
        const auto slash = name.rfind('/');
        e.depth = static_cast<size_t>(std::count(name.begin(), name.end(), '/'));
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        if (iequalsW(toWide(base), wanted) && (!best || e.depth < best->depth)) best = e;
        p += 46 + nameLen + extraLen + commentLen;
    }
    if (!best) fail(i18n::format(tr(L"Pakette {} yok."), {wanted}));
    if (best->flags & 1) fail(tr(L"Şifreli ZIP paketleri desteklenmiyor."));
    if (best->compressed == 0xffffffff || best->size == 0xffffffff) fail(tr(L"ZIP64 paketleri desteklenmiyor."));
    if (best->size > (512ull << 20)) fail(tr(L"Paket beklenenden çok büyük."));
    const size_t lh = best->local;
    if (lh + 30 > data.size() || rd32(data, lh) != 0x04034b50) fail(tr(L"Paket bozuk (yerel ZIP başlığı)."));
    const size_t start = lh + 30 + rd16(data, lh + 26) + rd16(data, lh + 28);
    if (start + best->compressed > data.size()) fail(tr(L"Paket bozuk (eksik veri)."));
    const std::string_view raw(data.data() + start, best->compressed);
    std::string content;
    if (best->method == 0) {
        if (best->compressed != best->size) fail(tr(L"Paket bozuk (boyut)."));
        content.assign(raw);
    } else if (best->method == 8) {
        content = inflate(raw, best->size);
    } else {
        fail(i18n::format(tr(L"Desteklenmeyen ZIP sıkıştırması ({})."), static_cast<long long>(best->method)));
    }
    if (crc32(content.data(), content.size()) != best->crc) fail(tr(L"Paket bozuk (CRC-32 tutmuyor)."));
    writeFile(out, content);
}

void verifyExe(const fs::path& exe, std::string_view expectedVersion) {
    std::ifstream f(exe, std::ios::binary);
    std::string head(4096, '\0');
    f.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(std::max<std::streamsize>(0, f.gcount())));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    bool ok = head.size() >= sizeof dos;
    if (ok) {
        std::memcpy(&dos, head.data(), sizeof dos);
        ok = dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && static_cast<size_t>(dos.e_lfanew) + sizeof nt <= head.size();
    }
    if (ok) {
        std::memcpy(&nt, head.data() + dos.e_lfanew, sizeof nt);
        ok = nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
             nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && (nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) &&
             !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) && nt.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
    }
    if (!ok) fail(tr(L"İndirilen dosya 64 bit bir Windows uygulaması değil."));
    if (const std::string name = fileProductName(exe); name != "ShadeTube")
        fail(name.empty() ? std::wstring(tr(L"İndirilen dosya ShadeTube değil (ürün adı yok)."))
                          : i18n::format(tr(L"İndirilen dosya ShadeTube değil ({})."), {toWide(name)}));
    const std::string version = fileProductVersion(exe);
    if (version.empty() || normalizeVersion(version) != normalizeVersion(expectedVersion)) {
        const std::wstring expected = toWide(normalizeVersion(expectedVersion));
        fail(version.empty() ? i18n::format(tr(L"İndirilen dosyanın sürümü bilinmiyor, beklenen {}."), {expected})
                             : i18n::format(tr(L"İndirilen dosyanın sürümü {}, beklenen {}."), {toWide(version), expected}));
    }
}

// ---- Leftover record ----------------------------------------------------------------------------------------

namespace {

std::mutex g_recordMutex;   // the record is read / rewritten from worker threads

fs::path recordPath() { return paths::appData() / L"update-leftovers.txt"; }

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return iequalsW(a.lexically_normal().wstring(), b.lexically_normal().wstring()) || fs::equivalent(a, b, ec);
}

std::vector<fs::path> readRecordLocked() {
    std::vector<fs::path> entries;
    std::ifstream f(recordPath(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) entries.emplace_back(toWide(line));
    }
    return entries;
}

void writeRecordLocked(const std::vector<fs::path>& entries) {
    std::error_code ec;
    if (entries.empty()) {
        fs::remove(recordPath(), ec);
        return;
    }
    const fs::path tmp = recordPath().wstring() + L".tmp";
    fs::create_directories(tmp.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& e : entries) f << toUtf8(e.wstring()) << '\n';
        f.close();
        if (f.fail()) fail(tr(L"Güncelleme kaydı yazılamadı."));
    }
    fs::rename(tmp, recordPath(), ec);   // replaces the old record in one step
    if (ec) fail(tr(L"Güncelleme kaydı yazılamadı."));
}

// Recorded BEFORE the file exists: if the process dies right after creating it, the next start still knows it is ours.
void recordLeftover(const fs::path& p) {
    std::lock_guard lock(g_recordMutex);
    auto entries = readRecordLocked();
    if (std::none_of(entries.begin(), entries.end(), [&](const fs::path& e) { return iequalsW(e.wstring(), p.wstring()); })) {
        entries.push_back(p);
        writeRecordLocked(entries);
    }
}

void forgetLeftover(const fs::path& p) {
    try {
        std::lock_guard lock(g_recordMutex);
        auto entries = readRecordLocked();
        const auto n = std::erase_if(entries, [&](const fs::path& e) { return iequalsW(e.wstring(), p.wstring()); });
        if (n) writeRecordLocked(entries);
    } catch (const std::exception& e) {
        ST_LOG_WARN("update", "leftover record: {}", e.what());   // a stale entry is harmless: it is re-checked later
    }
}

// `name` = prefix + digits (or digit groups joined by '-', when `groups`) + suffix, case-insensitive.
bool numbered(std::wstring_view name, std::wstring_view prefix, std::wstring_view suffix, bool groups) {
    if (name.size() <= prefix.size() + suffix.size() || !iequalsW(name.substr(0, prefix.size()), prefix) ||
        !iequalsW(name.substr(name.size() - suffix.size()), suffix))
        return false;
    const std::wstring_view mid = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    return mid.front() != L'-' && mid.back() != L'-' &&
           std::all_of(mid.begin(), mid.end(), [&](wchar_t c) { return (c >= L'0' && c <= L'9') || (groups && c == L'-'); });
}

} // namespace

std::vector<fs::path> recordedLeftovers() {
    std::lock_guard lock(g_recordMutex);
    return readRecordLocked();
}

bool isUpdaterFileName(const fs::path& name) {
    const std::wstring n = name.filename().wstring();
    return iequalsW(n, L"ShadeTube.old.exe") || numbered(n, L"ShadeTube.old-", L".exe", false) ||
           numbered(n, L"ShadeTube.new-", L".exe", true) || numbered(n, L"ShadeTube.update-", L".zip", true);
}

fs::path swapExe(const fs::path& target, const fs::path& replacement) {
    // The previous exe is parked under the first free name; existing files (a user's own ShadeTube.old.exe, a leftover
    // still in use) are never deleted or overwritten here.
    const fs::path dir = target.parent_path();
    fs::path old;
    for (int n = 1; n < 1000 && old.empty(); ++n) {
        const fs::path candidate = dir / (n == 1 ? std::wstring(L"ShadeTube.old.exe") : std::format(L"ShadeTube.old-{}.exe", n));
        std::error_code ec;
        if (!fs::exists(candidate, ec) && !ec) old = candidate;
    }
    if (old.empty()) fail(tr(L"Eski sürüm için boş bir dosya adı bulunamadı."));
    recordLeftover(old);
    if (!moveRetry(target, old)) {
        const DWORD err = GetLastError();
        forgetLeftover(old);
        fail(i18n::format(tr(L"ShadeTube.exe yeniden adlandırılamadı ({})."), {winError(err)}));
    }
    if (!moveRetry(replacement, target)) {
        const DWORD err = GetLastError();
        if (!moveRetry(old, target)) {
            const DWORD rollbackErr = GetLastError();
            ST_LOG_ERROR("update", "swap failed ({}) and the rollback failed too ({})", toUtf8(winError(err)),
                         toUtf8(winError(rollbackErr)));
            forgetLeftover(old);   // it is the only copy of the app now: never delete it
            fail(i18n::format(tr(L"Yeni sürüm yerine konamadı ve eski sürüm geri yüklenemedi: {} dosyasının adını {} olarak değiştir."),
                              {old.filename().wstring(), target.filename().wstring()}));
        }
        forgetLeftover(old);
        ST_LOG_WARN("update", "swap failed ({}), original restored", toUtf8(winError(err)));
        fail(i18n::format(tr(L"Yeni sürüm yerine konamadı ({}); mevcut sürüm korundu."), {winError(err)}));
    }
    ST_LOG_INFO("update", "swapped {} (previous exe parked as {})", toUtf8(target.wstring()), toUtf8(old.filename().wstring()));
    return old;
}

void downloadAndInstall(const Release& release, const fs::path& target, const ProgressFn& progress, const CancelToken& cancel) {
    const Asset* asset = pickAsset(release);
    if (!asset) fail(tr(L"Bu sürümde Windows paketi yok."));
    const fs::path dir = target.parent_path();
    const bool zip = endsWith(lower(asset->name), ".zip");
    // Everything is written next to the target (the final rename stays on one volume: atomic, no copy), under unique
    // names created here with CREATE_NEW: a file someone keeps next to the exe is never overwritten or deleted.
    const std::wstring tag = std::format(L"{}-{}", GetCurrentProcessId(), GetTickCount64());
    const fs::path fresh = dir / (L"ShadeTube.new-" + tag + L".exe");
    const fs::path downloaded = zip ? dir / (L"ShadeTube.update-" + tag + L".zip") : fresh;
    std::vector<fs::path> created;
    auto removeTemp = [&] {
        for (const auto& p : created) {
            std::error_code ec;
            if (!fs::exists(p, ec) || fs::remove(p, ec)) forgetLeftover(p);   // else: the next start retries
        }
        created.clear();
    };
    auto create = [&](const fs::path& p) {
        recordLeftover(p);
        const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            forgetLeftover(p);
            fail(i18n::format(tr(L"ShadeTube'un klasörüne yazılamıyor ({}). Yeni sürümü GitHub'dan elle indirebilirsin."),
                              {dir.wstring()}));
        }
        CloseHandle(h);
        created.push_back(p);
    };
    try {
        create(downloaded);   // also proves the folder is writable before anything is downloaded
        if (zip) create(fresh);
        ST_LOG_INFO("update", "downloading {} ({} bytes) from {}", asset->name, asset->size, asset->url);
        download(asset->url, downloaded, progress, cancel);
        const std::string hash = sha256File(downloaded);
        if (!asset->sha256.empty() && hash != asset->sha256) fail(tr(L"İndirilen dosyanın SHA-256 özeti GitHub'dakiyle tutmuyor."));
        if (!release.notesSha256.empty() && std::find(release.notesSha256.begin(), release.notesSha256.end(), hash) == release.notesSha256.end())
            fail(tr(L"İndirilen dosyanın SHA-256 özeti sürüm notlarındakiyle tutmuyor."));
        if (cancel.isCancellationRequested()) throw Cancelled();
        if (zip) {
            extractZipEntry(downloaded, "ShadeTube.exe", fresh);
            std::error_code ec;
            if (fs::remove(downloaded, ec)) {
                forgetLeftover(downloaded);
                std::erase(created, downloaded);
            }
        }
        verifyExe(fresh, release.version);
        if (cancel.isCancellationRequested()) throw Cancelled();
        swapExe(target, fresh);
    } catch (...) {
        removeTemp();
        throw;
    }
    // `fresh` is the target now; only a zip that could not be deleted above stays recorded.
    std::erase(created, fresh);
    forgetLeftover(fresh);
    removeTemp();
}

bool cleanupAfterUpdate(const fs::path& exe, int timeoutMs) {
    // A snapshot: files an update started meanwhile records (and still uses) are not this cleanup's business.
    std::vector<fs::path> todo;
    try {
        todo = recordedLeftovers();
    } catch (const std::exception& e) {
        ST_LOG_WARN("update", "leftover record: {}", e.what());
    }
    if (todo.empty()) return false;
    bool removed = false;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, timeoutMs));
    for (;;) {
        std::vector<fs::path> pending;
        for (const auto& p : todo) {
            if (samePath(p, exe)) continue;   // someone runs the parked exe: keep it (and its record) for now
            if (!isUpdaterFileName(p)) {      // never delete a name the updater does not create
                forgetLeftover(p);
                continue;
            }
            if (DeleteFileW(p.c_str())) {
                removed = true;
                forgetLeftover(p);
                continue;
            }
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) forgetLeftover(p);
            else pending.push_back(p);   // the previous process is still exiting
        }
        todo = std::move(pending);
        if (todo.empty() || GetTickCount64() >= deadline) {
            if (!todo.empty()) ST_LOG_WARN("update", "{} update leftover(s) still in use, retried at the next start", todo.size());
            else if (removed) ST_LOG_INFO("update", "removed the leftovers of an update");
            return removed;
        }
        Sleep(500);
    }
}

} // namespace st::app::updater
