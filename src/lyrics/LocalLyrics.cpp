// Lyrics stored with the user's own audio files: a sidecar .lrc, ID3v2 SYLT / USLT frames (read here) and, for the
// other formats, the Windows property system's System.Music.Lyrics.
#include "lyrics/Lyrics.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace st::lyrics {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxSidecarBytes = 2u << 20;   // a .lrc is a few KB; anything this big is not one
constexpr size_t kMaxTagBytes = 64u << 20;      // ID3 tags carry cover art; lyrics frames are read out of them

std::string_view trimView(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n\v\f");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

std::string readFile(const fs::path& p, size_t maxBytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string data;
    data.resize(maxBytes);
    f.read(data.data(), static_cast<std::streamsize>(maxBytes));
    data.resize(static_cast<size_t>(f.gcount()));
    return data;
}

std::string utf16ToUtf8(const uint8_t* p, size_t bytes, bool bigEndian) {
    std::wstring w;
    w.reserve(bytes / 2);
    for (size_t i = 0; i + 1 < bytes; i += 2)
        w.push_back(static_cast<wchar_t>(bigEndian ? (p[i] << 8 | p[i + 1]) : (p[i] | p[i + 1] << 8)));
    return toUtf8(w);
}

std::string latin1ToUtf8(const uint8_t* p, size_t n) {
    std::wstring w(p, p + n);   // ISO-8859-1 bytes are their code points
    return toUtf8(w);
}

// ---- ID3v2 -----------------------------------------------------------------------------------------------------

uint32_t synchsafe(const uint8_t* p) { return uint32_t(p[0] & 0x7F) << 21 | uint32_t(p[1] & 0x7F) << 14 | uint32_t(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Reverses the unsynchronisation scheme (every 0xFF 0x00 pair was written for a 0xFF).
std::string deunsync(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out.push_back(s[i]);
        if (static_cast<uint8_t>(s[i]) == 0xFF && i + 1 < s.size() && s[i + 1] == 0) ++i;
    }
    return out;
}

// A null-terminated string in frame `enc` starting at `pos`; advances `pos` past its terminator.
std::string readString(std::string_view d, size_t& pos, uint8_t enc) {
    const auto* p = reinterpret_cast<const uint8_t*>(d.data());
    const bool wide = enc == 1 || enc == 2;
    size_t end = pos;
    if (wide) {
        while (end + 1 < d.size() && !(p[end] == 0 && p[end + 1] == 0)) end += 2;
    } else {
        while (end < d.size() && p[end] != 0) ++end;
    }
    const size_t len = std::min(end, d.size()) - pos;
    std::string out;
    if (enc == 0) {
        out = latin1ToUtf8(p + pos, len);
    } else if (enc == 3) {
        out.assign(d.substr(pos, len));
    } else {
        bool be = enc == 2;
        size_t start = pos;
        if (enc == 1 && len >= 2) {
            if (p[pos] == 0xFE && p[pos + 1] == 0xFF) be = true, start += 2;
            else if (p[pos] == 0xFF && p[pos + 1] == 0xFE) start += 2;
        }
        out = utf16ToUtf8(p + start, pos + len - start, be);
    }
    pos = std::min(d.size(), end + (wide ? 2 : 1));
    return out;
}

std::optional<Lyrics> parseUslt(std::string_view d) {
    if (d.size() < 5) return std::nullopt;
    const uint8_t enc = static_cast<uint8_t>(d[0]);
    if (enc > 3) return std::nullopt;
    size_t pos = 4;
    readString(d, pos, enc);   // content descriptor
    // The text runs to the end of the frame (a terminator is optional).
    std::string text;
    {
        size_t p = pos;
        text = readString(d, p, enc);
    }
    if (trimView(text).empty()) return std::nullopt;
    Lyrics l = parseLrc(text);   // taggers often store LRC here
    if (l.lines.empty()) return std::nullopt;
    l.source = kSourceTag;
    return l;
}

std::optional<Lyrics> parseSylt(std::string_view d) {
    if (d.size() < 7) return std::nullopt;
    const uint8_t enc = static_cast<uint8_t>(d[0]);
    const uint8_t stampFormat = static_cast<uint8_t>(d[4]);
    if (enc > 3 || stampFormat != 2) return std::nullopt;   // 1 = MPEG frames: needs the frame rate, not supported
    size_t pos = 6;
    readString(d, pos, enc);   // content descriptor
    struct Entry {
        int ms;
        std::string text;
    };
    std::vector<Entry> entries;
    while (pos < d.size()) {
        std::string text = readString(d, pos, enc);
        if (pos + 4 > d.size()) break;
        const int ms = static_cast<int>(be32(reinterpret_cast<const uint8_t*>(d.data()) + pos));
        pos += 4;
        entries.push_back({ms, std::move(text)});
    }
    if (entries.empty()) return std::nullopt;
    // Karaoke taggers store syllables and mark a new line with a leading line break; otherwise an entry is a line.
    const bool syllables =
        std::any_of(entries.begin() + 1, entries.end(), [](const Entry& e) { return !e.text.empty() && (e.text[0] == '\n' || e.text[0] == '\r'); });
    Lyrics l;
    l.synced = true;
    l.source = kSourceTag;
    for (auto& e : entries) {
        const bool newLine = !e.text.empty() && (e.text[0] == '\n' || e.text[0] == '\r');
        std::string text = e.text;
        text.erase(0, text.find_first_not_of("\r\n"));
        if (!syllables || newLine || l.lines.empty()) {
            l.lines.push_back({e.ms, {}, {}});
            if (!syllables) {
                l.lines.back().text = std::string(trimView(text));
                continue;
            }
        }
        l.lines.back().words.push_back({e.ms, std::move(text)});
    }
    if (syllables) {
        for (auto& line : l.lines) {
            for (const auto& w : line.words) line.text += w.text;
            const auto lead = line.text.find_first_not_of(" \t");
            if (lead == std::string::npos) {
                line.text.clear();
                line.words.clear();
                continue;
            }
            line.text = std::string(trimView(line.text));
            if (line.words.size() < 2) line.words.clear();
            else {
                line.words.front().text.erase(0, line.words.front().text.find_first_not_of(" \t"));
                auto& tail = line.words.back().text;
                tail.erase(tail.find_last_not_of(" \t") + 1);
                std::erase_if(line.words, [](const Word& w) { return w.text.empty(); });
            }
        }
    }
    std::stable_sort(l.lines.begin(), l.lines.end(), [](const Line& a, const Line& b) { return a.timeMs < b.timeMs; });
    if (std::none_of(l.lines.begin(), l.lines.end(), [](const Line& x) { return !x.text.empty(); })) return std::nullopt;
    return l;
}

// ---- Windows property system -----------------------------------------------------------------------------------

std::string propertyLyrics(const fs::path& path) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE on an STA thread: not ours
    std::string out;
    {
        Microsoft::WRL::ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Music_Lyrics, &pv)) && pv.vt != VT_EMPTY) {
                PWSTR s = nullptr;
                if (SUCCEEDED(PropVariantToStringAlloc(pv, &s)) && s) out = toUtf8(s);
                CoTaskMemFree(s);
            }
            PropVariantClear(&pv);
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

std::optional<Lyrics> embedded(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    const std::string head = readFile(path, 10);
    if (head.size() == 10 && head.compare(0, 3, "ID3") == 0) {
        const uint32_t size = synchsafe(reinterpret_cast<const uint8_t*>(head.data()) + 6);
        const std::string tag = readFile(path, std::min<size_t>(kMaxTagBytes, size_t(size) + 10));
        if (auto l = readId3Lyrics(tag)) return l;
        if (ext == ".mp3") return std::nullopt;   // Windows reads the same frames
    }
    const std::string text = propertyLyrics(path);
    if (trimView(text).empty()) return std::nullopt;
    Lyrics l = parseLrc(text);
    if (l.lines.empty()) return std::nullopt;
    l.source = kSourceTag;
    return l;
}

} // namespace

std::string decodeText(const std::string& bytes) {
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return bytes.substr(3);
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) return utf16ToUtf8(p + 2, n - 2, false);
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) return utf16ToUtf8(p + 2, n - 2, true);
    if (n == 0) return {};
    if (n > static_cast<size_t>(INT_MAX)) return {};
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(n), nullptr, 0) > 0) return bytes;
    // Not UTF-8: an older tool's ANSI file (e.g. Windows-1254 for Turkish).
    const int len = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(n), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, len)), L'\0');
    if (len > 0) MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(n), w.data(), len);
    return toUtf8(w);
}

std::optional<Lyrics> readId3Lyrics(const std::string& bytes) {
    const auto* h = reinterpret_cast<const uint8_t*>(bytes.data());
    if (bytes.size() < 10 || bytes.compare(0, 3, "ID3") != 0) return std::nullopt;
    const uint8_t version = h[3], flags = h[5];
    if (version != 3 && version != 4) return std::nullopt;
    const size_t size = std::min<size_t>(synchsafe(h + 6), bytes.size() - 10);
    std::string body = bytes.substr(10, size);
    if (version == 3 && (flags & 0x80)) body = deunsync(body);   // v2.3: the whole tag is unsynchronised
    size_t pos = 0;
    if (flags & 0x40 && body.size() >= 4) {   // extended header
        const auto* e = reinterpret_cast<const uint8_t*>(body.data());
        pos = version == 3 ? 4 + be32(e) : synchsafe(e);
    }
    std::optional<Lyrics> sylt, uslt;
    while (pos + 10 <= body.size()) {
        const auto* f = reinterpret_cast<const uint8_t*>(body.data()) + pos;
        if (f[0] == 0) break;   // padding
        const uint32_t len = version == 4 ? synchsafe(f + 4) : be32(f + 4);
        const uint8_t fmt = f[9];
        if (len > body.size() - pos - 10) break;   // malformed
        std::string_view id(body.data() + pos, 4);
        std::string data = body.substr(pos + 10, len);
        pos += 10 + len;
        if (id != "SYLT" && id != "USLT") continue;
        if (version == 3) {
            if (fmt & 0xC0) continue;   // compressed / encrypted
            if (fmt & 0x20) data.erase(0, 1);   // group id
        } else {
            if (fmt & 0x0C) continue;   // compressed / encrypted
            if (fmt & 0x40) data.erase(0, 1);   // group id
            if ((fmt & 0x02) || (flags & 0x80)) data = deunsync(data);
            if (fmt & 0x01) data.erase(0, std::min<size_t>(4, data.size()));   // data length indicator
        }
        if (id == "SYLT" && !sylt) sylt = parseSylt(data);
        else if (id == "USLT" && !uslt) uslt = parseUslt(data);
    }
    if (sylt) return sylt;
    return uslt;
}

fs::path sidecarPath(const fs::path& audioPath) {
    fs::path p = audioPath;
    p.replace_extension(L".lrc");
    return p;
}

std::optional<Lyrics> readLocal(const fs::path& audioPath) {
    std::error_code ec;
    if (audioPath.empty() || !fs::is_regular_file(audioPath, ec)) return std::nullopt;
    std::optional<Lyrics> side;
    const fs::path lrc = sidecarPath(audioPath);
    if (fs::is_regular_file(lrc, ec)) {
        Lyrics l = parseLrc(decodeText(readFile(lrc, kMaxSidecarBytes)));
        if (!l.lines.empty()) {
            l.source = kSourceFile;
            if (l.synced) return l;
            side = std::move(l);
        }
    }
    std::optional<Lyrics> tag;
    try {
        tag = embedded(audioPath);
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyrics", "reading embedded lyrics failed: {}", e.what());
    }
    if (tag && tag->synced) return tag;
    if (side) return side;
    return tag;
}

} // namespace st::lyrics
