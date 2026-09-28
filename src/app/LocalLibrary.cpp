#include "app/LocalLibrary.h"

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>
// PKEY_* definitions (selectany) for this file.
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>
#include <unordered_set>

#pragma comment(lib, "propsys.lib")

namespace st::app {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace local {

namespace {

std::wstring lower(std::wstring_view s) {
    if (s.empty()) return {};
    std::wstring out(s);
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()), out.data(),
                  static_cast<int>(out.size()), nullptr, nullptr, 0);
    return out;
}

uint64_t fnv1a(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::string trim(std::string s) {
    const auto sp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (!s.empty() && sp(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && sp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// COM for the calling thread (MTA); a thread that is already STA keeps it (and we don't uninitialize it).
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

// ---- property store ---------------------------------------------------------------------------------

std::string propString(IPropertyStore* ps, REFPROPERTYKEY key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string out;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt != VT_EMPTY) {
        PWSTR s = nullptr;
        if (SUCCEEDED(PropVariantToStringAlloc(pv, &s)) && s) out = trim(toUtf8(s));
        CoTaskMemFree(s);
    }
    PropVariantClear(&pv);
    return out;
}

// Multi-valued strings (System.Music.Artist is a vector); a single value may still be "A; B".
std::vector<std::string> propStrings(IPropertyStore* ps, REFPROPERTYKEY key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::vector<std::string> raw;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt != VT_EMPTY) {
        if (pv.vt == (VT_VECTOR | VT_LPWSTR)) {
            for (ULONG i = 0; i < pv.calpwstr.cElems; ++i)
                if (pv.calpwstr.pElems[i]) raw.push_back(toUtf8(pv.calpwstr.pElems[i]));
        } else {
            PWSTR s = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(pv, &s)) && s) raw.push_back(toUtf8(s));
            CoTaskMemFree(s);
        }
    }
    PropVariantClear(&pv);
    std::vector<std::string> out;
    for (const auto& r : raw) {
        size_t start = 0;
        while (start <= r.size()) {
            const size_t semi = r.find(';', start);
            std::string part = trim(r.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
            if (!part.empty() && std::find(out.begin(), out.end(), part) == out.end()) out.push_back(std::move(part));
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }
    return out;
}

uint64_t propUInt(IPropertyStore* ps, REFPROPERTYKEY key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    ULONGLONG v = 0;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt != VT_EMPTY) PropVariantToUInt64(pv, &v);
    PropVariantClear(&pv);
    return v;
}

const wchar_t* imageExt(const std::string& b) {
    auto starts = [&](std::string_view sig, size_t at = 0) { return b.size() >= at + sig.size() && b.compare(at, sig.size(), sig) == 0; };
    if (starts("\xFF\xD8\xFF")) return L".jpg";
    if (starts("\x89PNG")) return L".png";
    if (starts("GIF8")) return L".gif";
    if (starts("BM")) return L".bmp";
    if (starts("RIFF") && starts("WEBP", 8)) return L".webp";
    return nullptr;   // not an image WIC decodes: ignore it
}

// ID3v2 APIC / PIC of an MP3, read directly: Windows' handler drops pictures whose frame doesn't follow the tag
// version to the letter (e.g. a v2.3 APIC with UTF-8 = encoding 3, which only v2.4 defines). Front cover preferred.
std::string id3Picture(const std::wstring& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    unsigned char h[10];
    if (!f.read(reinterpret_cast<char*>(h), 10) || h[0] != 'I' || h[1] != 'D' || h[2] != '3' || h[3] < 2 || h[3] > 4) return {};
    const int ver = h[3];
    const uint32_t tagSize = (h[6] & 0x7Fu) << 21 | (h[7] & 0x7Fu) << 14 | (h[8] & 0x7Fu) << 7 | (h[9] & 0x7Fu);
    if (tagSize == 0 || tagSize > 32u * 1024 * 1024) return {};
    std::string tag(tagSize, '\0');
    if (!f.read(tag.data(), tagSize)) return {};
    auto deunsync = [](std::string& s) {   // 0xFF 0x00 -> 0xFF
        size_t o = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            s[o++] = s[i];
            if (static_cast<unsigned char>(s[i]) == 0xFF && i + 1 < s.size() && s[i + 1] == 0) ++i;
        }
        s.resize(o);
    };
    if ((h[5] & 0x80) && ver < 4) deunsync(tag);
    auto be = [&](size_t at, int n, bool syncsafe) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = syncsafe ? (v << 7) | (static_cast<unsigned char>(tag[at + i]) & 0x7Fu) : (v << 8) | static_cast<unsigned char>(tag[at + i]);
        return v;
    };
    size_t pos = 0;
    if ((h[5] & 0x40) && ver >= 3 && tag.size() >= 4)   // extended header
        pos = ver == 4 ? be(0, 4, true) : be(0, 4, false) + 4;
    const size_t headLen = ver == 2 ? 6 : 10;
    std::string best;
    while (pos + headLen <= tag.size() && tag[pos] != 0) {
        const std::string id = tag.substr(pos, ver == 2 ? 3 : 4);
        const uint32_t size = ver == 2 ? be(pos + 3, 3, false) : be(pos + 4, 4, ver == 4);
        const int fmtFlags = ver == 2 ? 0 : static_cast<unsigned char>(tag[pos + 9]);
        pos += headLen;
        if (size == 0 || size > tag.size() - pos) break;
        if (id == "APIC" || id == "PIC") {
            std::string d = tag.substr(pos, size);
            if (ver == 4 && (fmtFlags & 0x02)) deunsync(d);
            if (ver == 4 && (fmtFlags & 0x01) && d.size() >= 4) d.erase(0, 4);   // data length indicator
            if (ver == 4 && (fmtFlags & 0x0C)) {   // compressed / encrypted: not supported
                pos += size;
                continue;
            }
            const int enc = d.empty() ? 0 : static_cast<unsigned char>(d[0]);
            size_t p = 1;
            if (id == "PIC") p += 3;                                  // "JPG" / "PNG"
            else p = d.find('\0', 1) == std::string::npos ? d.size() : d.find('\0', 1) + 1;   // mime
            const int type = p < d.size() ? static_cast<unsigned char>(d[p]) : 0;
            ++p;
            if (enc == 1 || enc == 2) {   // UTF-16 description: double-NUL terminated
                while (p + 1 < d.size() && !(d[p] == 0 && d[p + 1] == 0)) p += 2;
                p += 2;
            } else {
                while (p < d.size() && d[p] != 0) ++p;
                ++p;
            }
            if (p < d.size() && (best.empty() || type == 3)) {
                best = d.substr(p);
                if (type == 3) break;
            }
        }
        pos += size;
    }
    return best;
}

// Embedded picture as the property handlers expose it (ID3 APIC / MP4 covr / FLAC PICTURE / WM/Picture).
std::string thumbnailBytes(IPropertyStore* ps) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string bytes;
    if (SUCCEEDED(ps->GetValue(PKEY_ThumbnailStream, &pv)) && pv.vt == VT_STREAM && pv.pStream) {
        STATSTG st{};
        const uint64_t size = SUCCEEDED(pv.pStream->Stat(&st, STATFLAG_NONAME)) ? st.cbSize.QuadPart : 0;
        if (size > 0 && size <= 16u * 1024 * 1024) {
            LARGE_INTEGER zero{};
            pv.pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            bytes.resize(static_cast<size_t>(size));
            ULONG got = 0;
            if (FAILED(pv.pStream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &got))) got = 0;
            bytes.resize(got);
        }
    }
    PropVariantClear(&pv);
    return bytes;
}

// Written once as <hash of the bytes>.<ext>: an album's tracks share one file. "" when `bytes` isn't an image.
std::wstring saveCover(const std::string& bytes, const fs::path& coverDir) {
    const wchar_t* ext = imageExt(bytes);
    if (!ext) return {};
    const fs::path file = coverDir / (toWide(hex64(fnv1a(bytes.data(), bytes.size()))) + ext);
    std::error_code ec;
    if (fs::exists(file, ec)) return file.wstring();
    fs::create_directories(coverDir, ec);
    // Another reader thread may write the same cover: private temp name, then rename.
    fs::path tmp = file;
    tmp += L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) return {};
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (!fs::exists(file, ec)) return {};
    }
    return file.wstring();
}

// ---- file name fallback ("01 - Artist - Title.mp3") ----------------------------------------------------

bool isDash(wchar_t c) { return c == L'-' || c == L'\u2013' || c == L'\u2014'; }

// Up to 3 ASCII digits after optional spaces ("2/2" -> 2); 0 when there are none. (iswdigit would also accept
// superscripts and other scripts' digits, which don't parse as numbers.)
int leadingNumber(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    int n = 0;
    for (size_t d = 0; i < s.size() && d < 3 && s[i] >= L'0' && s[i] <= L'9'; ++i, ++d) n = n * 10 + (s[i] - L'0');
    return n;
}

void fromFileName(Entry& e) {
    std::wstring stem = fs::path(e.path).stem().wstring();
    if (stem.find(L' ') == std::wstring::npos) std::replace(stem.begin(), stem.end(), L'_', L' ');
    // Leading track number: "01 - ", "01 – ", "1. ", "01) ", "07_", or a zero-padded "01 " ("50 Cent" stays).
    size_t d = 0;
    int number = 0;
    while (d < stem.size() && d < 3 && stem[d] >= L'0' && stem[d] <= L'9') number = number * 10 + (stem[d++] - L'0');
    if (d > 0 && d < stem.size()) {
        size_t rest = std::wstring::npos;
        const wchar_t c = stem[d];
        if (c == L'.' || c == L')' || c == L'_' || isDash(c)) rest = d + 1;
        else if (c == L' ' && d + 2 < stem.size() && isDash(stem[d + 1]) && stem[d + 2] == L' ') rest = d + 3;
        else if (c == L' ' && stem[0] == L'0') rest = d + 1;
        if (rest != std::wstring::npos) {
            while (rest < stem.size() && (stem[rest] == L' ' || isDash(stem[rest]))) ++rest;
            if (rest < stem.size()) {
                if (e.trackNumber == 0) e.trackNumber = number;
                stem = stem.substr(rest);
            }
        }
    }
    // "Artist - Title": split at the earliest " - " / " – " / " — ".
    std::wstring artist, title = stem;
    size_t at = std::wstring::npos, sepLen = 0;
    for (const wchar_t* sep : {L" - ", L" \u2013 ", L" \u2014 "}) {
        const size_t pos = stem.find(sep);
        if (pos != std::wstring::npos && pos > 0 && pos < at) {
            at = pos;
            sepLen = wcslen(sep);
        }
    }
    if (at != std::wstring::npos) {
        artist = stem.substr(0, at);
        title = stem.substr(at + sepLen);
    }
    if (e.title.empty()) e.title = trim(toUtf8(title.empty() ? stem : title));
    if (e.artists.empty() && e.albumArtist.empty() && !artist.empty()) e.artists.push_back(trim(toUtf8(artist)));
}

// Disc from the album's folder name: "CD2", "CD 2", "Disc 2", "Disk 2 - Bonus" -> 2; 0 otherwise.
int discFromFolder(const std::wstring& filePath) {
    const std::wstring name = lower(fs::path(filePath).parent_path().filename().wstring());
    for (const wchar_t* prefix : {L"cd", L"disc", L"disk"})
        if (name.starts_with(prefix)) return leadingNumber(std::wstring_view(name).substr(wcslen(prefix)));
    return 0;
}

// ---- paths ---------------------------------------------------------------------------------------------

// Names the UTF-8 model (index, ids, file URLs, settings) can hold: no unpaired UTF-16 surrogates.
bool validUtf16(std::wstring_view s) {
    return s.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                            nullptr, nullptr) > 0;
}

// Folders the walk never enters: junctions / symlinks / mount points (name surrogates: loops; cloud placeholder
// folders such as OneDrive's ARE walked), hidden or system folders ($RECYCLE.BIN), "." folders (.git) and names the
// index can't store.
bool skipDir(DWORD attrs, DWORD reparseTag, std::wstring_view name) {
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(reparseTag)) return true;
    if (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return true;
    return name.empty() || name.front() == L'.' || !validUtf16(name);
}

// "C:\" / "\\server\share\" of a path ("" when it has neither form).
std::wstring volumeRoot(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return path.substr(0, 2) + L"\\";
    if (path.starts_with(L"\\\\")) {
        const size_t server = path.find(L'\\', 2);
        if (server == std::wstring::npos) return path + L"\\";
        const size_t share = path.find(L'\\', server + 1);
        return (share == std::wstring::npos ? path : path.substr(0, share)) + L"\\";
    }
    return {};
}

// Listing `dir` failed with `err`. true: it is only unreachable right now (volume / share offline, access denied,
// device error) and its indexed tracks must be kept; false: it is empty, or really gone from a reachable volume.
bool unreachable(const std::wstring& dir, DWORD err) {
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) return false;   // empty (a drive root has no ".")
    if (err == ERROR_PATH_NOT_FOUND || err == ERROR_DIRECTORY || err == ERROR_INVALID_NAME) {
        const std::wstring root = volumeRoot(dir);
        return root.empty() || GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES;
    }
    return true;
}

// Both arguments already lower-cased.
bool underLower(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || path.size() < folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    return path.size() == folder.size() || folder.back() == L'\\' || path[folder.size()] == L'\\';
}

// Folder images next to the files (cover.jpg, folder.jpg, ...), looked up once per directory and scan.
class FolderArt {
public:
    std::wstring find(const std::wstring& filePath) {
        const std::wstring dir = fs::path(filePath).parent_path().wstring();
        const std::wstring key = lower(dir);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        std::wstring found;
        for (const wchar_t* name : {L"cover", L"folder", L"front", L"album"}) {
            for (const wchar_t* ext : {L".jpg", L".jpeg", L".png"}) {
                const std::wstring p = openablePath(dir + L"\\" + name + ext);   // long: ImageCache opens it as is
                const DWORD a = GetFileAttributesW(p.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                    found = p;
                    break;
                }
            }
            if (!found.empty()) break;
        }
        cache_.emplace(key, found);
        return found;
    }

private:
    std::unordered_map<std::wstring, std::wstring> cache_;
};

bool canceled(const ScanOptions& o) { return o.cancel && o.cancel->requested(); }

bool extraHandler(const wchar_t* ext) {
    // Formats beyond the Windows defaults (Ogg / Opus) need a Media Foundation byte-stream handler (e.g. from a
    // codec extension); without one the files wouldn't play, so they aren't listed.
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers\\") + ext;
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
            RegCloseKey(h);
            return true;
        }
    }
    return false;
}

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------------------------------

const std::vector<std::wstring>& audioExtensions() {
    static const std::vector<std::wstring> exts = [] {
        std::vector<std::wstring> v{L".mp3", L".m4a", L".aac", L".flac", L".wav", L".wma"};
        for (const wchar_t* e : {L".ogg", L".oga", L".opus"})
            if (extraHandler(e)) v.push_back(e);
        return v;
    }();
    return exts;
}

bool isAudioFile(const std::wstring& path) {
    const std::wstring ext = lower(fs::path(path).extension().wstring());
    const auto& all = audioExtensions();
    return std::find(all.begin(), all.end(), ext) != all.end();
}

std::string trackIdFor(const std::wstring& path) {
    const std::string key = toUtf8(lower(path));
    return "local:" + hex64(fnv1a(key.data(), key.size()));
}

std::string fileUrl(const std::wstring& path) {
    std::string p = toUtf8(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out = "file:///";
    static const char* hexd = "0123456789ABCDEF";
    for (unsigned char c : p) {
        if (isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hexd[c >> 4];
            out += hexd[c & 15];
        }
    }
    return out;
}

std::wstring openablePath(const std::wstring& path) {
    constexpr size_t kPlainLimit = 240;   // under MAX_PATH with room for "\*" / a file name
    if (path.size() < kPlainLimit || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring normalizeFolder(const std::wstring& path) {
    if (path.empty() || !validUtf16(path)) return {};
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec);
    if (ec) return {};
    std::wstring s = p.lexically_normal().wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

bool isUnder(const std::wstring& path, const std::wstring& folder) { return underLower(lower(path), lower(folder)); }

bool walkedFrom(const std::wstring& parent, const std::wstring& child) {
    const std::wstring p = normalizeFolder(parent), c = normalizeFolder(child);
    if (p.empty() || c.empty() || !isUnder(c, p)) return false;
    // Every level below `parent`, down to `child` itself, must be a folder the walk enters.
    for (size_t pos = p.size(); pos < c.size();) {
        if (c[pos] == L'\\') ++pos;
        const size_t next = c.find(L'\\', pos);
        const std::wstring level = c.substr(0, next);
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileExW(openablePath(level).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return false;
        FindClose(h);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || skipDir(fd.dwFileAttributes, fd.dwReserved0, fd.cFileName)) return false;
        if (next == std::wstring::npos) break;
        pos = next;
    }
    return true;
}

void ScanCancel::request() {
    flag_.store(true);
    std::lock_guard lock(mutex_);
    for (void* t : threads_) CancelSynchronousIo(static_cast<HANDLE>(t));   // a stalled CreateFile / listing / read
}

ScanCancel::ThreadScope::ThreadScope(ScanCancel* cancel) : cancel_(cancel) {
    if (!cancel_) return;
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    thread_ = h;
    std::lock_guard lock(cancel_->mutex_);
    cancel_->threads_.push_back(thread_);
}

ScanCancel::ThreadScope::~ThreadScope() {
    if (!thread_) return;
    {
        std::lock_guard lock(cancel_->mutex_);
        std::erase(cancel_->threads_, thread_);
    }
    CloseHandle(static_cast<HANDLE>(thread_));
}

bool readFile(Entry& e, const fs::path& coverDir, const ScanCancel* cancel) {
    const auto stop = [cancel] { return cancel && cancel->requested(); };
    // Locked / access denied / vanished: skip. Opened for reading only, sharing everything.
    const std::wstring open = openablePath(e.path);
    const HANDLE h = CreateFileW(open.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    if (stop()) return false;

    e.title.clear();
    e.artists.clear();
    e.albumArtist.clear();
    e.album.clear();
    e.discNumber = e.trackNumber = e.year = e.durationMs = 0;
    e.tagged = false;
    e.cover.clear();
    e.embeddedCover = false;

    ComPtr<IPropertyStore> ps;
    std::string picture;
    HRESULT hr = SHGetPropertyStoreFromParsingName(e.path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps));
    if (FAILED(hr) && open != e.path) hr = SHGetPropertyStoreFromParsingName(open.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps));
    if (SUCCEEDED(hr)) {
        e.title = propString(ps.Get(), PKEY_Title);
        e.artists = propStrings(ps.Get(), PKEY_Music_Artist);
        e.albumArtist = propString(ps.Get(), PKEY_Music_AlbumArtist);
        e.album = propString(ps.Get(), PKEY_Music_AlbumTitle);
        e.discNumber = static_cast<int>(std::min<uint64_t>(propUInt(ps.Get(), PKEY_Music_DiscNumber), 999));
        if (e.discNumber == 0) e.discNumber = leadingNumber(toWide(propString(ps.Get(), PKEY_Music_PartOfSet)));   // "2/2"
        e.trackNumber = static_cast<int>(std::min<uint64_t>(propUInt(ps.Get(), PKEY_Music_TrackNumber), 9999));
        e.year = static_cast<int>(std::min<uint64_t>(propUInt(ps.Get(), PKEY_Media_Year), 9999));
        e.durationMs = static_cast<int>(std::min<uint64_t>(propUInt(ps.Get(), PKEY_Media_Duration) / 10'000, INT32_MAX));
        e.tagged = !e.title.empty();
        if (!coverDir.empty()) picture = thumbnailBytes(ps.Get());
    }
    if (stop()) return false;
    if (!coverDir.empty()) {
        if (!imageExt(picture) && _wcsicmp(fs::path(e.path).extension().c_str(), L".mp3") == 0) picture = id3Picture(open);
        if (stop()) return false;
        e.cover = saveCover(picture, coverDir);
        e.embeddedCover = !e.cover.empty();
    }
    if (e.discNumber == 0) e.discNumber = discFromFolder(e.path);
    if (e.artists.empty() && !e.albumArtist.empty()) e.artists.push_back(e.albumArtist);
    fromFileName(e);
    return true;
}

std::vector<Entry> scan(const ScanOptions& o, const std::vector<Entry>& previous, ScanStats* statsOut) {
    ScanStats stats;
    const auto started = std::chrono::steady_clock::now();
    const ScanCancel::ThreadScope cancelScope(o.cancel.get());   // cancel() interrupts this thread's blocking I/O
    const auto giveUp = [&]() -> std::vector<Entry> {
        stats.cancelled = true;
        if (statsOut) *statsOut = stats;
        return {};
    };

    // 1. Walk the folders (Win32 directly: attributes, size and mtime come with the listing).
    struct Found {
        std::wstring path;
        int64_t size;
        uint64_t mtime;
        bool carried;   // its folder couldn't be listed: the previous entry is kept as is
    };
    std::vector<Found> found;
    std::vector<std::wstring> unlisted;      // folders that couldn't be listed (completely or partly)
    std::unordered_set<std::wstring> seen;   // lower-cased paths (nested / duplicate folders)
    for (const auto& folder : o.folders) {
        if (stats.capped || canceled(o)) break;
        const std::wstring root = normalizeFolder(folder);
        if (root.empty()) continue;
        std::vector<std::wstring> dirs{root};
        while (!dirs.empty() && !stats.capped && !canceled(o)) {
            const std::wstring dir = std::move(dirs.back());
            dirs.pop_back();
            const std::wstring prefix = dir.back() == L'\\' ? dir : dir + L'\\';
            WIN32_FIND_DATAW fd;
            const HANDLE h = FindFirstFileExW(openablePath(prefix + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                              nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) {
                const DWORD err = GetLastError();
                if (!canceled(o) && unreachable(dir, err)) {
                    unlisted.push_back(dir);
                    if (dir == root) stats.missingRoots.push_back(root);
                    ST_LOG_WARN("local", "can't list {} (error {}): its indexed tracks are kept", toUtf8(dir), err);
                }
                continue;
            }
            bool stopped = false;   // left early on purpose (cap / cancel), not a listing error
            do {
                if (canceled(o)) {
                    stopped = true;
                    break;
                }
                const std::wstring_view name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                const DWORD a = fd.dwFileAttributes;
                if (a & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!skipDir(a, fd.dwReserved0, name)) dirs.push_back(prefix + std::wstring(name));
                    else if (a & FILE_ATTRIBUTE_REPARSE_POINT) ST_LOG_DEBUG("local", "link not followed: {}{}", toUtf8(prefix), toUtf8(name));
                    continue;
                }
                if (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                if (name.starts_with(L"._") || !validUtf16(name)) continue;   // macOS resource forks / unstorable names
                std::wstring path = prefix + std::wstring(name);
                if (!isAudioFile(path)) continue;
                // Cloud placeholders (OneDrive "online-only"): reading tags would download every file.
                if (a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
                    ++stats.onlineOnly;
                    continue;
                }
                const int64_t size = (static_cast<int64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                if (size <= 0 || size > kMaxFileBytes) continue;
                if (!seen.insert(lower(path)).second) continue;
                if (found.size() >= o.maxFiles) {
                    stats.capped = stopped = true;
                    break;
                }
                const uint64_t mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                found.push_back({std::move(path), size, mtime, false});
            } while (FindNextFileW(h, &fd));
            const DWORD err = stopped ? ERROR_NO_MORE_FILES : GetLastError();
            FindClose(h);
            if (err != ERROR_NO_MORE_FILES && !canceled(o)) {   // the listing broke off (network error): keep the rest
                unlisted.push_back(dir);
                ST_LOG_WARN("local", "listing {} stopped early (error {}): its indexed tracks are kept", toUtf8(dir), err);
            }
        }
    }
    if (stats.capped) ST_LOG_WARN("local", "more than {} audio files in the music folders: the rest is skipped", o.maxFiles);
    if (stats.onlineOnly > 0) ST_LOG_INFO("local", "{} online-only (cloud) files skipped", stats.onlineOnly);
    if (canceled(o)) return giveUp();

    // A folder that couldn't be listed is not empty: its indexed files stay as they were.
    if (!unlisted.empty()) {
        stats.incomplete = true;
        std::vector<std::wstring> lowered;
        for (const auto& u : unlisted) lowered.push_back(lower(u));
        for (const auto& e : previous) {
            const std::wstring key = lower(e.path);
            if (seen.contains(key) ||
                std::none_of(lowered.begin(), lowered.end(), [&](const std::wstring& u) { return underLower(key, u); }))
                continue;
            seen.insert(key);
            found.push_back({e.path, e.size, e.mtime, true});
            ++stats.carried;
        }
    }

    // 2. Unchanged files keep their index entry; the rest is read.
    std::unordered_map<std::wstring, const Entry*> prev;
    prev.reserve(previous.size());
    for (const auto& e : previous) prev.emplace(lower(e.path), &e);
    std::vector<Entry> entries(found.size());
    std::vector<size_t> todo;
    for (size_t i = 0; i < found.size(); ++i) {
        auto& e = entries[i];
        const auto it = prev.find(lower(found[i].path));
        std::error_code ec;
        if (found[i].carried && it != prev.end()) {
            e = *it->second;
            if (e.embeddedCover && !fs::exists(e.cover, ec)) {   // can't re-extract it now
                e.cover.clear();
                e.embeddedCover = false;
            }
        } else if (it != prev.end() && it->second->size == found[i].size && it->second->mtime == found[i].mtime &&
                   (!it->second->embeddedCover || fs::exists(it->second->cover, ec))) {
            e = *it->second;
            e.path = found[i].path;
            ++stats.reused;
        } else {
            e.path = found[i].path;
            e.size = found[i].size;
            e.mtime = found[i].mtime;
            todo.push_back(i);
        }
    }
    stats.removed = static_cast<int>(std::count_if(previous.begin(), previous.end(), [&](const Entry& e) {
        return !seen.contains(lower(e.path));
    }));

    // 3. Read tags (+ covers) of new / changed files on a few threads.
    std::vector<char> ok(entries.size(), 1);
    if (!todo.empty()) {
        std::atomic<size_t> next{0};
        std::atomic<int> done{0};
        const int total = static_cast<int>(todo.size());
        if (o.progress) o.progress(0, total);
        auto reader = [&] {
            ComScope com;
            const ScanCancel::ThreadScope scope(o.cancel.get());
            for (size_t k; (k = next.fetch_add(1)) < todo.size();) {
                if (canceled(o)) break;
                const size_t i = todo[k];
                try {
                    ok[i] = readFile(entries[i], o.coverDir, o.cancel.get()) ? 1 : 0;
                } catch (...) {
                    ok[i] = 0;
                }
                const int d = done.fetch_add(1) + 1;
                if (o.progress) o.progress(d, total);
            }
        };
        const int helpers = std::clamp(std::min(o.readThreads, total / 32 + 1), 1, 8) - 1;
        std::vector<std::jthread> threads;
        for (int t = 0; t < helpers; ++t) threads.emplace_back(reader);
        reader();
        threads.clear();   // joins
        if (canceled(o)) return giveUp();
        stats.read = total;
    }
    std::vector<Entry> out;
    out.reserve(entries.size());
    FolderArt folderArt;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!ok[i]) {
            ++stats.failed;
            ST_LOG_INFO("local", "unreadable, skipped: {}", toUtf8(entries[i].path));
            continue;
        }
        auto& e = entries[i];
        if (!e.embeddedCover && !found[i].carried) e.cover = folderArt.find(e.path);
        out.push_back(std::move(e));
    }
    stats.read -= std::min(stats.read, stats.failed);

    // 4. Artist (album artist first, so compilations stay together), album, disc, track, title.
    struct Key {
        std::wstring artist, album, title;
    };
    std::vector<Key> keys(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const auto& e = out[i];
        keys[i] = {toWide(!e.albumArtist.empty() ? e.albumArtist : e.artists.empty() ? std::string{} : e.artists[0]),
                   toWide(e.album), toWide(e.title)};
    }
    auto cmp = [](const std::wstring& a, const std::wstring& b) {
        if (a.empty() != b.empty()) return a.empty() ? 1 : -1;   // untagged last
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                               static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
               CSTR_EQUAL;
    };
    std::vector<size_t> order(out.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (int c = cmp(keys[x].artist, keys[y].artist)) return c < 0;
        if (int c = cmp(keys[x].album, keys[y].album)) return c < 0;
        const int dx = std::max(out[x].discNumber, 1), dy = std::max(out[y].discNumber, 1);   // untagged disc = disc 1
        if (dx != dy) return dx < dy;
        if (out[x].trackNumber != out[y].trackNumber) return out[x].trackNumber < out[y].trackNumber;
        if (int c = cmp(keys[x].title, keys[y].title)) return c < 0;
        return out[x].path < out[y].path;
    });
    std::vector<Entry> sorted;
    sorted.reserve(out.size());
    for (size_t i : order) sorted.push_back(std::move(out[i]));

    stats.files = static_cast<int>(sorted.size());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    ST_LOG_INFO("local", "scan: {} files ({} unchanged, {} read, {} unreadable, {} removed, {} kept from unreachable folders) in {} ms",
                stats.files, stats.reused, stats.read, stats.failed, stats.removed, stats.carried, ms);
    if (statsOut) *statsOut = stats;
    return sorted;
}

catalog::Track toTrack(const Entry& e) {
    catalog::Track t;
    t.id = trackIdFor(e.path);
    t.name = e.title;
    for (const auto& a : e.artists) t.artists.push_back({{}, a});   // no ids: local artists open nothing online
    t.album.name = e.album;
    if (!e.cover.empty()) t.album.images.push_back({fileUrl(e.cover), 0, 0});
    t.durationMs = e.durationMs;
    t.trackNumber = e.trackNumber;
    // FILETIME (100 ns since 1601) -> unix seconds: the file's date stands in for "added".
    if (e.mtime > 116444736000000000ULL) t.addedAt = static_cast<int64_t>((e.mtime - 116444736000000000ULL) / 10'000'000ULL);
    return t;
}

// ---- index ------------------------------------------------------------------------------------------
// {"version":2,"scannedAt":<unix>,"folders":[...],"capped":false,"files":[[path,size,mtime,title,[artists],albumArtist,
//  album,disc,track,year,durMs,tagged,cover,embeddedCover], ...]} (arrays keep a large library's index small to parse).
// Version 1 (no disc column) is not read: every file is read once again and gets its disc number.

std::vector<Entry> loadIndex(const fs::path& file, IndexMeta* meta) {
    std::vector<Entry> out;
    std::ifstream f(file, std::ios::binary);
    if (!f) return out;
    const json j = json::parse(f, nullptr, false);
    if (!j.is_object() || j.value("version", 0) != 2) return out;
    if (meta) {
        meta->scannedAt = j.value("scannedAt", int64_t{0});
        meta->capped = j.value("capped", false);
        meta->folders.clear();
        for (const auto& fo : j.value("folders", json::array()))
            if (fo.is_string()) meta->folders.push_back(toWide(fo.get<std::string>()));
    }
    const auto files = j.find("files");
    if (files == j.end() || !files->is_array()) return out;
    out.reserve(files->size());
    for (const auto& a : *files) {
        if (!a.is_array() || a.size() < 14 || !a[0].is_string()) continue;
        try {
            Entry e;
            e.path = toWide(a[0].get<std::string>());
            e.size = a[1].get<int64_t>();
            e.mtime = a[2].get<uint64_t>();
            e.title = a[3].get<std::string>();
            for (const auto& ar : a[4])
                if (ar.is_string()) e.artists.push_back(ar.get<std::string>());
            e.albumArtist = a[5].get<std::string>();
            e.album = a[6].get<std::string>();
            e.discNumber = a[7].get<int>();
            e.trackNumber = a[8].get<int>();
            e.year = a[9].get<int>();
            e.durationMs = a[10].get<int>();
            e.tagged = a[11].get<bool>();
            e.cover = toWide(a[12].get<std::string>());
            e.embeddedCover = a[13].get<bool>();
            if (!e.path.empty()) out.push_back(std::move(e));
        } catch (const json::exception&) {
            // a malformed row: dropped (re-read by the next scan)
        }
    }
    return out;
}

bool saveIndex(const fs::path& file, const std::vector<Entry>& entries, const IndexMeta& meta) {
    json files = json::array();
    for (const auto& e : entries)
        files.push_back(json::array({toUtf8(e.path), e.size, e.mtime, e.title, e.artists, e.albumArtist, e.album, e.discNumber,
                                     e.trackNumber, e.year, e.durationMs, e.tagged, toUtf8(e.cover), e.embeddedCover}));
    json folders = json::array();
    for (const auto& fo : meta.folders) folders.push_back(toUtf8(fo));
    const json j = {{"version", 2},
                    {"scannedAt", meta.scannedAt},
                    {"folders", std::move(folders)},
                    {"capped", meta.capped},
                    {"files", std::move(files)}};
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(-1, ' ', false, json::error_handler_t::replace);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

int pruneCovers(const fs::path& coverDir, const std::vector<Entry>& entries) {
    std::unordered_set<std::wstring> used;
    for (const auto& e : entries)
        if (e.embeddedCover) used.insert(lower(e.cover));
    int removed = 0;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(coverDir, ec)) {
        if (!de.is_regular_file(ec) || used.contains(lower(de.path().wstring()))) continue;
        if (fs::remove(de.path(), ec)) ++removed;
    }
    return removed;
}

} // namespace local

// ===================================================================================================
// LocalLibrary (UI thread)

namespace {

fs::path indexFile() { return paths::appData() / L"local-library.json"; }
fs::path coverDir() { return paths::cacheDir() / L"local-covers"; }

} // namespace

struct LocalLibrary::Snapshot {
    std::shared_ptr<const std::vector<local::Entry>> entries;
    std::vector<catalog::Track> tracks;
    std::unordered_map<std::string, std::wstring> paths;
    int64_t totalMs = 0;
    local::IndexMeta meta;       // of the index / of the scan that produced it
    local::ScanStats stats;
    bool changed = true;
};

namespace {

// `before` (optional): the snapshot being replaced, to tell whether anything changed.
std::shared_ptr<LocalLibrary::Snapshot> makeSnapshot(std::vector<local::Entry> entries, const std::vector<local::Entry>* before) {
    auto s = std::make_shared<LocalLibrary::Snapshot>();
    if (before && before->size() == entries.size()) {
        s->changed = false;
        for (size_t i = 0; i < entries.size() && !s->changed; ++i) {
            const auto &a = entries[i], &b = (*before)[i];
            s->changed = a.path != b.path || a.size != b.size || a.mtime != b.mtime || a.cover != b.cover;
        }
    }
    s->tracks.reserve(entries.size());
    for (const auto& e : entries) {
        s->tracks.push_back(local::toTrack(e));
        s->paths.emplace(s->tracks.back().id, e.path);
        s->totalMs += e.durationMs;
    }
    s->entries = std::make_shared<const std::vector<local::Entry>>(std::move(entries));
    return s;
}

std::vector<std::wstring> listedFolders() {
    std::vector<std::wstring> out;
    for (const auto& f : Settings::get().localFolders) out.push_back(toWide(f));
    return out;
}

bool sameFolder(const std::wstring& a, const std::wstring& b) { return local::isUnder(a, b) && local::isUnder(b, a); }

} // namespace

LocalLibrary& LocalLibrary::get() {
    static LocalLibrary instance;
    return instance;
}

void LocalLibrary::notify() {
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    auto snapshot = listeners_;
    for (auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

void LocalLibrary::apply(std::shared_ptr<Snapshot> s) {
    // Only what the folders listed NOW cover: an index or a scan from before a folder was removed must not bring its
    // tracks back.
    std::vector<std::wstring> roots;
    for (const auto& f : listedFolders()) roots.push_back(local::lower(f));
    const auto listed = [&](const local::Entry& e) {
        const std::wstring p = local::lower(e.path);
        return std::any_of(roots.begin(), roots.end(), [&](const std::wstring& r) { return local::underLower(p, r); });
    };
    if (!std::all_of(s->entries->begin(), s->entries->end(), listed)) {
        std::vector<local::Entry> keep;
        std::copy_if(s->entries->begin(), s->entries->end(), std::back_inserter(keep), listed);
        s = makeSnapshot(std::move(keep), entries_.get());
    }
    if (!s->changed) return;
    entries_ = s->entries;
    tracks_ = std::move(s->tracks);
    paths_ = std::move(s->paths);
    totalMs_ = s->totalMs;
    ++generation_;
}

void LocalLibrary::start() {
    if (started_) return;
    started_ = true;
    const fs::path file = indexFile();
    async(
        Priority::Normal, life_.ref(),
        [file] {
            local::IndexMeta meta;
            auto s = makeSnapshot(local::loadIndex(file, &meta), nullptr);
            s->meta = std::move(meta);
            return s;
        },
        [this](Result<std::shared_ptr<Snapshot>> r) {
            loaded_ = true;
            if (r) {
                const auto& meta = (*r)->meta;
                lastScanAt_ = meta.scannedAt;
                indexCapped_ = meta.capped;
                if (meta.scannedAt > 0 && scannedFolders_.empty()) scannedFolders_ = meta.folders;
                apply(*r);
                ST_LOG_INFO("local", "index: {} tracks", tracks_.size());
            }
            // A little after launch: pick up what changed on disk while the app was closed.
            if (!Settings::get().localFolders.empty() || !tracks_.empty()) startupScanDue_ = static_cast<int64_t>(GetTickCount64()) + 4000;
            if (rescanQueued_) {
                rescanQueued_ = false;
                startScan();
            }
            notify();
        });
}

void LocalLibrary::tick() {
    if (startupScanDue_ > 0 && static_cast<int64_t>(GetTickCount64()) >= startupScanDue_) {
        startupScanDue_ = 0;
        rescan();
    }
}

void LocalLibrary::rescan() {
    if (!loaded_ || scanning_) {
        rescanQueued_ = true;   // after the index loads / the running scan
        return;
    }
    startScan();
}

void LocalLibrary::cancel() {
    rescanQueued_ = false;
    startupScanDue_ = 0;
    if (cancel_) cancel_->request();
}

void LocalLibrary::startScan() {
    scanning_ = true;
    startupScanDue_ = 0;
    scanDone_ = scanTotal_ = 0;
    cancel_ = std::make_shared<local::ScanCancel>();

    local::ScanOptions o;
    for (const auto& f : listedFolders())
        if (std::wstring n = local::normalizeFolder(f); !n.empty()) o.folders.push_back(std::move(n));
    o.coverDir = coverDir();
    o.cancel = cancel_;
    // Progress: at most ~8 UI updates per second (plus the last one).
    auto lastPost = std::make_shared<std::atomic<uint64_t>>(0);
    o.progress = [this, ref = life_.ref(), lastPost](int done, int total) {
        const uint64_t now = GetTickCount64();
        if (done > 0 && done < total && now - lastPost->load() < 120) return;
        lastPost->store(now);
        Dispatcher::post([this, ref, done, total] {
            if (ref.expired() || !scanning_) return;
            scanDone_ = done;
            scanTotal_ = total;
            notify();
        });
    };
    const auto before = entries_;
    const fs::path file = indexFile();
    async(
        Priority::Low, life_.ref(),
        [o, before, file] {
            local::ScanStats st;
            auto entries = local::scan(o, *before, &st);
            if (st.cancelled) {
                auto s = std::make_shared<Snapshot>();
                s->stats = st;
                return s;
            }
            const local::IndexMeta meta{local::unixNow(), o.folders, st.capped};
            if (!local::saveIndex(file, entries, meta)) ST_LOG_WARN("local", "could not write {}", toUtf8(file.wstring()));
            // Covers of files the scan didn't see (capped / a folder that couldn't be listed) may still be in use.
            if (!st.capped && !st.incomplete) local::pruneCovers(o.coverDir, entries);
            auto s = makeSnapshot(std::move(entries), before.get());
            s->stats = st;
            s->meta = meta;
            return s;
        },
        [this](Result<std::shared_ptr<Snapshot>> r) {
            scanning_ = false;
            cancel_.reset();
            if (!r) {
                ST_LOG_WARN("local", "scan failed: {}", r.errorMessage());
            } else if (!(*r)->stats.cancelled) {
                stats_ = (*r)->stats;
                scanned_ = true;
                lastScanAt_ = (*r)->meta.scannedAt;
                scannedFolders_ = (*r)->meta.folders;
                apply(*r);
            }
            if (rescanQueued_) {
                rescanQueued_ = false;
                startScan();   // notifies below with scanning_ = true again
            }
            notify();
        });
    notify();
}

std::wstring LocalLibrary::pathFor(const std::string& trackId) const {
    const auto it = paths_.find(trackId);
    return it == paths_.end() ? std::wstring{} : local::openablePath(it->second);
}

int LocalLibrary::countUnder(const std::wstring& folder) const {
    const std::wstring f = local::lower(folder);
    return static_cast<int>(std::count_if(entries_->begin(), entries_->end(),
                                          [&](const local::Entry& e) { return local::underLower(local::lower(e.path), f); }));
}

bool LocalLibrary::scannedFolder(const std::wstring& folder) const {
    return std::any_of(scannedFolders_.begin(), scannedFolders_.end(), [&](const std::wstring& s) { return local::isUnder(folder, s); });
}

bool LocalLibrary::folderUnavailable(const std::wstring& folder) const {
    return std::any_of(stats_.missingRoots.begin(), stats_.missingRoots.end(), [&](const std::wstring& m) { return sameFolder(m, folder); });
}

bool LocalLibrary::addFolder(const std::wstring& path) {
    const std::wstring n = local::normalizeFolder(path);
    if (n.empty()) return false;
    auto& folders = Settings::get().localFolders;
    for (const auto& f : folders) {
        const std::wstring w = toWide(f);
        if (sameFolder(n, w) || local::walkedFrom(w, n)) return false;   // a listed folder's scan already covers it
    }
    // Listed folders the new one walks into are covered by it now (a junction / hidden one inside stays listed).
    std::erase_if(folders, [&](const std::string& f) { return local::walkedFrom(n, toWide(f)); });
    folders.push_back(toUtf8(n));
    Settings::get().markDirty();
    ST_LOG_INFO("local", "folder added ({} total)", folders.size());
    rescan();
    notify();
    return true;
}

void LocalLibrary::removeFolder(const std::wstring& path) {
    // A running scan may be reading that folder: stop it (a cancelled scan is neither saved nor applied) and scan
    // the remaining folders again.
    if (scanning_ && cancel_) cancel_->request();
    const std::wstring n = local::normalizeFolder(path);
    auto& folders = Settings::get().localFolders;
    std::erase_if(folders, [&](const std::string& f) { return sameFolder(toWide(f), n); });
    Settings::get().markDirty();
    // Its tracks leave right away, unless another listed folder still scans them: one it is walked from, or a
    // listed folder inside it (the rescan confirms it and rewrites the index).
    bool coveredByOther = false;
    std::vector<std::wstring> inner;
    for (const auto& f : listedFolders()) {
        if (local::walkedFrom(f, n)) coveredByOther = true;
        else if (local::isUnder(f, n)) inner.push_back(local::lower(f));
    }
    if (!coveredByOther) {
        const std::wstring removed = local::lower(n);
        std::vector<local::Entry> keep;
        for (const auto& e : *entries_) {
            const std::wstring p = local::lower(e.path);
            if (!local::underLower(p, removed) ||
                std::any_of(inner.begin(), inner.end(), [&](const std::wstring& i) { return local::underLower(p, i); }))
                keep.push_back(e);
        }
        if (keep.size() != entries_->size()) apply(makeSnapshot(std::move(keep), nullptr));
    }
    rescan();
    notify();
}

} // namespace st::app
