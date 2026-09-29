#include "app/PlaylistIO.h"

#include "app/Links.h"
#include "core/Utf.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace st::app::playlistio {

using catalog::Track;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kSpotifyTrack = "spotify:track:";
constexpr size_t kMaxFileBytes = 64u << 20;

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

bool iStartsWith(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
    return true;
}

std::string artistNames(const Track& t, std::string_view sep) {
    std::string s;
    for (const auto& a : t.artists) {
        if (a.name.empty()) continue;
        if (!s.empty()) s += sep;
        s += a.name;
    }
    return s;
}

// "Artist A, Artist B - Title" (the usual #EXTINF text), "Title" without artists.
std::string displayLine(const Track& t) {
    const std::string a = artistNames(t, ", ");
    return a.empty() ? t.name : a + " - " + t.name;
}

std::string mmss(int ms) {
    if (ms <= 0) return {};
    const int s = ms / 1000;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

std::string isoTime(int64_t unix) {
    if (unix <= 0) return {};
    const time_t t = static_cast<time_t>(unix);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return {};
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::string xmlEscape(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&': r += "&amp;"; break;
        case '<': r += "&lt;"; break;
        case '>': r += "&gt;"; break;
        case '"': r += "&quot;"; break;
        case '\'': r += "&apos;"; break;
        default:
            // XML 1.0 has no place for most control characters.
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
            r += c;
        }
    }
    return r;
}

std::string percentEncode(std::string_view s, bool keepSlash) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string r;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && (c == '/' || c == ':'))) {
            r += static_cast<char>(c);
        } else {
            r += '%';
            r += hex[c >> 4];
            r += hex[c & 15];
        }
    }
    return r;
}

std::string percentDecode(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            r += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            r += s[i];
        }
    }
    return r;
}

std::string videoOf(const Row& r) {
    if (!r.videoId.empty()) return r.videoId;
    if (!r.track.videoId.empty()) return r.track.videoId;
    if (r.track.id.rfind("yt:", 0) == 0) return r.track.id.substr(3);
    return {};
}

// A song known by name only (no id at all).
Track nameOnly(std::string title, std::string artist) {
    Track t;
    t.id = importId(title, artist);
    t.name = std::move(title);
    if (!artist.empty()) t.artists.push_back({"", std::move(artist)});
    return t;
}

// "Artist - Title" (M3U #EXTINF, a bare text line) -> artist, title. Without " - " the whole text is the title.
std::pair<std::string, std::string> splitDisplay(std::string_view text) {
    const std::string s = trim(text);
    const size_t dash = s.find(" - ");
    if (dash == std::string::npos) return {{}, s};
    return {trim(std::string_view(s).substr(0, dash)), trim(std::string_view(s).substr(dash + 3))};
}

// Applies what a link names to `t` (a song only). False for links to something else (an album, a playlist...).
bool applyLink(const links::Link& l, Track& t) {
    switch (l.kind) {
    case links::Kind::SpotifyTrack: t.id = std::string(kSpotifyTrack) + l.id; return true;
    case links::Kind::YouTubeVideo:
        t.id = "yt:" + l.id;
        t.videoId = l.id;
        return true;
    case links::Kind::MbRecording: t.id = l.id; return true;
    default: return false;
    }
}

bool isAbsolutePath(const fs::path& p) { return p.is_absolute() || p.has_root_name(); }

std::string resolvePath(std::string_view text, const fs::path& baseDir) {
    fs::path p(toWide(text));
    if (!isAbsolutePath(p) && !baseDir.empty()) p = baseDir / p;
    return toUtf8(p.lexically_normal().wstring());
}

std::string stemOf(const std::string& path) { return toUtf8(fs::path(toWide(path)).stem().wstring()); }

std::string shorten(std::string s) {
    if (s.size() > 120) {
        s.resize(117);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();   // whole characters
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
        s += "...";
    }
    return s;
}

// ---- M3U ------------------------------------------------------------------------------------------------------------
Parsed parseM3u(std::string_view text, const fs::path& baseDir) {
    Parsed p;
    p.format = Format::M3U;
    struct Info {
        int seconds = -1;
        std::string display, album, artist;
    } info;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        ++lineNo;
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (iStartsWith(line, "#EXTINF:")) {
                const std::string_view rest = std::string_view(line).substr(8);
                const size_t comma = rest.find(',');
                info.seconds = std::atoi(std::string(rest.substr(0, comma)).c_str());   // "-1 tvg-id=..." -> -1
                info.display = comma == std::string_view::npos ? std::string{} : trim(rest.substr(comma + 1));
            } else if (iStartsWith(line, "#PLAYLIST:")) {
                p.name = trim(std::string_view(line).substr(10));
            } else if (iStartsWith(line, "#EXTALB:")) {
                info.album = trim(std::string_view(line).substr(8));
            } else if (iStartsWith(line, "#EXTART:")) {
                info.artist = trim(std::string_view(line).substr(8));
            }
            continue;
        }
        Entry e;
        e.line = lineNo;
        auto [artist, title] = splitDisplay(info.display);
        if (!info.artist.empty()) artist = info.artist;
        Track& t = e.track;
        t.name = title;
        if (!artist.empty()) t.artists.push_back({"", artist});
        t.album.name = info.album;
        if (info.seconds > 0) t.durationMs = info.seconds * 1000;
        const bool uri = line.find("://") != std::string::npos || iStartsWith(line, "spotify:");
        if (iStartsWith(line, "file:")) {
            e.localPath = pathFromFileUri(line);
        } else if (uri) {
            const auto link = links::parse(line);
            if (!applyLink(link, t)) {
                // A link to an album / playlist / artist is no song; any other URL (a stream, a search) is, by the
                // #EXTINF name.
                const bool other = link && link.kind != links::Kind::Unsupported;
                if (other || t.name.empty()) {
                    p.unresolved.push_back({lineNo, shorten(line)});
                    info = {};
                    continue;
                }
                t = nameOnly(t.name, artist);
                t.album.name = info.album;
                if (info.seconds > 0) t.durationMs = info.seconds * 1000;
            }
        } else {
            e.localPath = resolvePath(line, baseDir);
        }
        if (!e.localPath.empty() && t.name.empty()) t.name = stemOf(e.localPath);
        p.entries.push_back(std::move(e));
        info = {};
    }
    return p;
}

// ---- CSV ------------------------------------------------------------------------------------------------------------
int findColumn(const std::vector<std::string>& header, std::initializer_list<std::string_view> names) {
    for (const auto n : names)
        for (size_t i = 0; i < header.size(); ++i)
            if (header[i] == n) return static_cast<int>(i);
    return -1;
}

// A cell as written: ShadeTube's formula guard ("'=...") comes off.
std::string cellText(const std::vector<std::string>& row, int col) {
    if (col < 0 || col >= static_cast<int>(row.size())) return {};
    std::string v = trim(row[static_cast<size_t>(col)]);
    if (v.size() >= 2 && v[0] == '\'' && (v[1] == '=' || v[1] == '+' || v[1] == '-' || v[1] == '@')) v.erase(0, 1);
    return v;
}

std::vector<std::string> splitArtists(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string::npos) end = s.size();
        std::string a = trim(std::string_view(s).substr(start, end - start));
        if (!a.empty()) out.push_back(std::move(a));
        start = end + 1;
    }
    return out;
}

Parsed parseCsv(std::string_view text, const fs::path& baseDir) {
    Parsed p;
    p.format = Format::CSV;
    const auto rows = readCsv(text);
    if (rows.empty()) return p;
    std::vector<std::string> header;
    for (const auto& h : rows[0]) header.push_back(lower(trim(h)));
    const int cTitle = findColumn(header, {"track name", "title", "name", "song", "song name", "track title", "track"});
    const int cArtist = findColumn(header, {"artist name(s)", "artist name", "artists", "artist", "artist(s)", "creator"});
    const int cAlbum = findColumn(header, {"album name", "album", "album title", "release"});
    const int cDurMs = findColumn(header, {"duration (ms)", "track duration (ms)", "duration_ms", "length (ms)"});
    const int cDur = findColumn(header, {"duration", "length", "time"});
    const int cSpotify = findColumn(header, {"spotify uri", "track uri", "uri", "spotify - id", "spotify id",
                                             "spotify track id", "spotify url", "spotify link"});
    const int cMbid = findColumn(header, {"musicbrainz id", "mbid", "recording mbid", "musicbrainz recording id"});
    const int cYouTube = findColumn(header, {"youtube video id", "youtube id", "video id", "youtube - id", "youtube"});
    const int cUrl = findColumn(header, {"url", "link"});
    const int cAdded = findColumn(header, {"added at", "added", "date added"});
    const int cFile = findColumn(header, {"file", "path", "location"});
    const bool hasHeader = cTitle >= 0 || cSpotify >= 0 || cYouTube >= 0 || cUrl >= 0 || cFile >= 0 || cMbid >= 0;
    char artistSep = ';';
    if (!hasHeader) p.dialect = "lines";
    else if (cSpotify >= 0 && cYouTube >= 0 && header[static_cast<size_t>(cSpotify)] == "spotify uri") p.dialect = "shadetube";
    else if (header[static_cast<size_t>(std::max(cArtist, 0))] == "artist name(s)" || findColumn(header, {"track uri"}) >= 0) {
        p.dialect = "exportify";
        artistSep = ',';   // Exportify joins the credits with commas
    } else if (findColumn(header, {"spotify - id", "playlist name"}) >= 0 || (cTitle >= 0 && header[static_cast<size_t>(cTitle)] == "track name"))
        p.dialect = "tunemymusic";
    else if (cTitle >= 0 && header[static_cast<size_t>(cTitle)] == "title" && cArtist >= 0 && header[static_cast<size_t>(cArtist)] == "artist")
        p.dialect = "soundiiz";
    else p.dialect = "generic";

    for (size_t r = hasHeader ? 1 : 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        const int lineNo = static_cast<int>(r) + 1;
        Entry e;
        e.line = lineNo;
        Track& t = e.track;
        std::vector<std::string> artists;
        if (!hasHeader) {   // no header we know: each row "Artist - Title" (a plain list of songs)
            auto [artist, title] = splitDisplay(row.empty() ? std::string{} : row[0]);
            t.name = title;
            if (!artist.empty()) artists.push_back(artist);
        } else {
            t.name = cellText(row, cTitle);
            artists = splitArtists(cellText(row, cArtist), artistSep);
            t.album.name = cellText(row, cAlbum);
            if (const std::string ms = cellText(row, cDurMs); !ms.empty()) t.durationMs = parseDurationMs(ms, true);
            else t.durationMs = parseDurationMs(cellText(row, cDur), false);
            t.addedAt = parseIsoTime(cellText(row, cAdded));
        }
        for (auto& a : artists) t.artists.push_back({"", std::move(a)});
        // The best id the row names: Spotify, MusicBrainz, a YouTube video, a link, a file, else the name.
        std::string id;
        if (const std::string sp = cellText(row, cSpotify); !sp.empty()) {
            if (sp.rfind(kSpotifyTrack, 0) == 0 && links::isSpotifyId(sp.substr(kSpotifyTrack.size()))) id = sp;
            else if (links::isSpotifyId(sp)) id = std::string(kSpotifyTrack) + sp;
            else if (const auto l = links::parse(sp); l.kind == links::Kind::SpotifyTrack) id = std::string(kSpotifyTrack) + l.id;
        }
        if (id.empty())
            if (const std::string mb = cellText(row, cMbid); links::isMbid(mb)) id = lower(mb);
        if (id.empty()) {
            const std::string yt = cellText(row, cYouTube);
            if (links::isVideoId(yt)) {
                id = "yt:" + yt;
                t.videoId = yt;
            } else if (!yt.empty()) {
                Track probe;
                if (applyLink(links::parse(yt), probe)) {
                    id = probe.id;
                    t.videoId = probe.videoId;
                }
            }
        } else if (const std::string yt = cellText(row, cYouTube); links::isVideoId(yt)) {
            t.videoId = yt;   // keeps the matched video of a Spotify / MusicBrainz song
        }
        if (id.empty())
            if (const std::string url = cellText(row, cUrl); !url.empty()) {
                Track probe;
                if (applyLink(links::parse(url), probe)) {
                    id = probe.id;
                    if (!probe.videoId.empty()) t.videoId = probe.videoId;
                }
            }
        if (id.empty())
            if (const std::string file = cellText(row, cFile); !file.empty()) {
                e.localPath = iStartsWith(file, "file:") ? pathFromFileUri(file) : resolvePath(file, baseDir);
                if (t.name.empty()) t.name = stemOf(e.localPath);
            }
        if (id.empty() && e.localPath.empty()) {
            if (t.name.empty()) {
                std::string raw;
                for (const auto& c : row) {
                    if (c.empty()) continue;
                    if (!raw.empty()) raw += ", ";
                    raw += c;
                }
                if (!raw.empty()) p.unresolved.push_back({lineNo, shorten(raw)});
                continue;
            }
            id = importId(t.name, t.artists.empty() ? std::string{} : t.artists[0].name);
        }
        t.id = id;
        p.entries.push_back(std::move(e));
    }
    return p;
}

// ---- XSPF -----------------------------------------------------------------------------------------------------------
std::string xmlDecode(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.substr(i, 9) == "<![CDATA[") {
            const size_t end = s.find("]]>", i + 9);
            const size_t stop = end == std::string_view::npos ? s.size() : end;
            r.append(s.substr(i + 9, stop - i - 9));
            i = end == std::string_view::npos ? s.size() : end + 3;
            continue;
        }
        if (s[i] == '&') {
            const size_t semi = s.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                const std::string_view ent = s.substr(i + 1, semi - i - 1);
                std::string out;
                if (ent == "amp") out = "&";
                else if (ent == "lt") out = "<";
                else if (ent == "gt") out = ">";
                else if (ent == "quot") out = "\"";
                else if (ent == "apos") out = "'";
                else if (!ent.empty() && ent[0] == '#') {
                    const unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                                 ? std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)
                                                 : std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
                    if (cp > 0 && cp < 0x110000) {
                        std::wstring w;
                        if (cp >= 0x10000) {
                            w += static_cast<wchar_t>(0xD800 + ((cp - 0x10000) >> 10));
                            w += static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
                        } else {
                            w += static_cast<wchar_t>(cp);
                        }
                        out = toUtf8(w);
                    }
                }
                if (!out.empty()) {
                    r += out;
                    i = semi + 1;
                    continue;
                }
            }
        }
        r += s[i++];
    }
    return trim(r);
}

// The text of the first <tag>...</tag> in `block` ("" when absent or empty).
std::string xmlChild(std::string_view block, std::string_view tag) {
    const std::string open = "<" + std::string(tag);
    size_t at = 0;
    while ((at = block.find(open, at)) != std::string_view::npos) {
        const size_t after = at + open.size();
        if (after < block.size() && (block[after] == '>' || block[after] == ' ' || block[after] == '/' || block[after] == '\t')) break;
        at = after;
    }
    if (at == std::string_view::npos) return {};
    const size_t gt = block.find('>', at);
    if (gt == std::string_view::npos || block[gt - 1] == '/') return {};
    const size_t close = block.find("</" + std::string(tag), gt);
    if (close == std::string_view::npos) return {};
    return xmlDecode(block.substr(gt + 1, close - gt - 1));
}

Parsed parseXspf(std::string_view text) {
    Parsed p;
    p.format = Format::XSPF;
    const size_t list = text.find("<trackList");
    p.name = xmlChild(text.substr(0, list == std::string_view::npos ? text.size() : list), "title");
    size_t at = list == std::string_view::npos ? 0 : list;
    int n = 0;
    while ((at = text.find("<track", at)) != std::string_view::npos) {
        const size_t after = at + 6;
        if (after >= text.size() || (text[after] != '>' && text[after] != ' ')) {   // <trackList>, <trackNum>
            at = after;
            continue;
        }
        const size_t end = text.find("</track>", after);
        if (end == std::string_view::npos) break;
        const std::string_view block = text.substr(after, end - after);
        at = end + 8;
        Entry e;
        e.line = ++n;
        Track& t = e.track;
        t.name = xmlChild(block, "title");
        if (const std::string c = xmlChild(block, "creator"); !c.empty()) t.artists.push_back({"", c});
        t.album.name = xmlChild(block, "album");
        t.durationMs = parseDurationMs(xmlChild(block, "duration"), true);
        const std::string location = xmlChild(block, "location"), identifier = xmlChild(block, "identifier");
        bool named = false;
        for (const std::string& ref : {identifier, location}) {
            if (ref.empty() || named) continue;
            if (iStartsWith(ref, "file:")) continue;
            named = applyLink(links::parse(ref), t);
        }
        if (!named && iStartsWith(location, "file:")) e.localPath = pathFromFileUri(location);
        else if (!named && !location.empty() && location.find("://") == std::string::npos) e.localPath = location;
        if (!named && e.localPath.empty()) {
            if (t.name.empty()) {
                p.unresolved.push_back({n, shorten(location.empty() ? identifier : location)});
                continue;
            }
            t.id = importId(t.name, t.artists.empty() ? std::string{} : t.artists[0].name);
        }
        if (!e.localPath.empty() && t.name.empty()) t.name = stemOf(e.localPath);
        p.entries.push_back(std::move(e));
    }
    return p;
}

// ---- JSON -----------------------------------------------------------------------------------------------------------
Parsed parseJson(std::string_view text) {
    Parsed p;
    p.format = Format::JSON;
    const json j = json::parse(text, nullptr, false);
    const json* tracks = nullptr;
    if (j.is_object()) {
        p.name = j.value("name", "");
        if (auto it = j.find("tracks"); it != j.end() && it->is_array()) tracks = &*it;
    } else if (j.is_array()) {
        tracks = &j;
    }
    if (!tracks) return p;
    int n = 0;
    for (const auto& o : *tracks) {
        ++n;
        if (!o.is_object()) continue;
        Entry e;
        e.line = n;
        Track& t = e.track;
        t.id = o.value("id", "");
        t.videoId = o.value("videoId", "");
        t.name = o.value("name", o.value("title", ""));
        if (auto a = o.find("artists"); a != o.end() && a->is_array()) {
            for (const auto& x : *a)
                if (x.is_object() && !x.value("name", "").empty()) t.artists.push_back({x.value("id", ""), x.value("name", "")});
                else if (x.is_string()) t.artists.push_back({"", x.get<std::string>()});
        } else if (const std::string artist = o.value("artist", ""); !artist.empty()) {
            t.artists.push_back({"", artist});
        }
        if (auto al = o.find("album"); al != o.end() && al->is_object()) {
            t.album.id = al->value("id", "");
            t.album.name = al->value("name", "");
            if (auto im = al->find("images"); im != al->end() && im->is_array())
                for (const auto& i : *im)
                    if (i.is_object() && !i.value("u", "").empty()) t.album.images.push_back({i.value("u", ""), i.value("w", 0), i.value("h", 0)});
        } else if (al != o.end() && al->is_string()) {
            t.album.name = al->get<std::string>();
        }
        t.durationMs = o.value("durationMs", 0);
        t.explicitContent = o.value("explicit", false);
        t.trackNumber = o.value("trackNumber", 0);
        t.addedAt = o.value("addedAt", int64_t{0});
        const std::string file = o.value("file", "");
        // A local file is known by its path (its id is a hash of the path on the PC that wrote the list).
        if (t.id.empty() || t.id.rfind("local:", 0) == 0) {
            t.id.clear();
            if (!file.empty()) e.localPath = file;
        }
        if (t.id.empty() && e.localPath.empty()) {
            if (t.name.empty()) {
                p.unresolved.push_back({n, shorten(o.dump())});
                continue;
            }
            t.id = importId(t.name, t.artists.empty() ? std::string{} : t.artists[0].name);
        }
        if (t.id.rfind("yt:", 0) == 0 && t.videoId.empty()) t.videoId = t.id.substr(3);
        p.entries.push_back(std::move(e));
    }
    return p;
}

} // namespace

// ---- Formats --------------------------------------------------------------------------------------------------------

const char* extension(Format f) {
    switch (f) {
    case Format::M3U: return ".m3u8";
    case Format::CSV: return ".csv";
    case Format::XSPF: return ".xspf";
    case Format::JSON: return ".json";
    }
    return ".txt";
}

std::optional<Format> formatFromPath(const fs::path& p) {
    const std::string ext = lower(toUtf8(p.extension().wstring()));
    if (ext == ".m3u" || ext == ".m3u8") return Format::M3U;
    if (ext == ".csv" || ext == ".tsv") return Format::CSV;
    if (ext == ".xspf") return Format::XSPF;
    if (ext == ".json") return Format::JSON;
    return std::nullopt;
}

Format sniff(std::string_view text) {
    const std::string head = trim(text.substr(0, 512));
    if (iStartsWith(head, "#EXTM3U")) return Format::M3U;
    if (!head.empty() && (head[0] == '{' || head[0] == '[')) return Format::JSON;
    if (head.find("<playlist") != std::string::npos) return Format::XSPF;
    return Format::CSV;
}

// ---- Writing --------------------------------------------------------------------------------------------------------

std::string trackUrl(const Row& r) {
    const std::string& id = r.track.id;
    if (id.rfind(kSpotifyTrack, 0) == 0) return "https://open.spotify.com/track/" + id.substr(kSpotifyTrack.size());
    if (links::isMbid(id)) return "https://musicbrainz.org/recording/" + id;
    if (const std::string v = videoOf(r); !v.empty()) return "https://www.youtube.com/watch?v=" + v;
    return {};
}

std::string fileUri(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.rfind("//", 0) == 0) return "file:" + percentEncode(p, true);   // UNC: file://server/share/...
    return "file:///" + percentEncode(p, true);
}

std::string pathFromFileUri(std::string_view uri) {
    if (!iStartsWith(uri, "file:")) return {};
    std::string_view rest = uri.substr(5);
    std::string path;
    if (rest.rfind("///", 0) == 0) path = percentDecode(rest.substr(3));          // file:///C:/x
    else if (rest.rfind("//", 0) == 0) {
        const std::string_view hostPath = rest.substr(2);
        if (iStartsWith(hostPath, "localhost/")) path = percentDecode(hostPath.substr(10));
        else path = "//" + percentDecode(hostPath);                                // file://server/share/x (UNC)
    } else {
        path = percentDecode(rest);
    }
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

std::string writeM3u8(const std::string& name, const std::vector<Row>& rows) {
    std::string out = "#EXTM3U\r\n";
    if (!name.empty()) out += "#PLAYLIST:" + name + "\r\n";
    for (const auto& r : rows) {
        const int sec = r.track.durationMs > 0 ? (r.track.durationMs + 500) / 1000 : -1;
        out += "#EXTINF:" + std::to_string(sec) + "," + displayLine(r.track) + "\r\n";
        if (!r.track.album.name.empty()) out += "#EXTALB:" + r.track.album.name + "\r\n";
        std::string where = r.filePath.empty() ? trackUrl(r) : r.filePath;
        if (where.empty())   // a song known by name only: a search, so the line still leads somewhere
            where = "https://music.youtube.com/search?q=" + percentEncode(artistNames(r.track, " ") + " " + r.track.name, false);
        out += where + "\r\n";
    }
    return out;
}

std::string writeCsv(const std::vector<Row>& rows) {
    auto cell = [](std::string v) {
        // A spreadsheet would run "=...", "+...", "-..." and "@..." as a formula (a crafted playlist title...).
        if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@')) v.insert(0, 1, '\'');
        const bool quote = v.find_first_of(",\"\r\n") != std::string::npos || (!v.empty() && (v.front() == ' ' || v.back() == ' '));
        if (!quote) return v;
        std::string q = "\"";
        for (const char c : v) {
            if (c == '"') q += '"';
            q += c;
        }
        return q + "\"";
    };
    std::string out = "\xEF\xBB\xBF";
    out += "Title,Artists,Album,Duration,Duration (ms),Spotify URI,MusicBrainz ID,YouTube video ID,Added at,File\r\n";
    for (const auto& r : rows) {
        const Track& t = r.track;
        const bool spotify = t.id.rfind(kSpotifyTrack, 0) == 0;
        const std::vector<std::string> cells{
            t.name, artistNames(t, "; "), t.album.name, mmss(t.durationMs),
            t.durationMs > 0 ? std::to_string(t.durationMs) : std::string{}, spotify ? t.id : std::string{},
            links::isMbid(t.id) ? t.id : std::string{}, videoOf(r), isoTime(t.addedAt), r.filePath};
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) out += ',';
            out += cell(cells[i]);
        }
        out += "\r\n";
    }
    return out;
}

std::string writeXspf(const std::string& name, const std::vector<Row>& rows) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n";
    if (!name.empty()) out += "  <title>" + xmlEscape(name) + "</title>\n";
    out += "  <creator>ShadeTube</creator>\n  <trackList>\n";
    for (const auto& r : rows) {
        const Track& t = r.track;
        out += "    <track>\n";
        const std::string url = trackUrl(r);
        if (!r.filePath.empty()) out += "      <location>" + xmlEscape(fileUri(r.filePath)) + "</location>\n";
        if (!url.empty()) out += "      <location>" + xmlEscape(url) + "</location>\n";
        if (t.id.rfind(kSpotifyTrack, 0) == 0 || links::isMbid(t.id)) out += "      <identifier>" + xmlEscape(url) + "</identifier>\n";
        out += "      <title>" + xmlEscape(t.name) + "</title>\n";
        if (const std::string a = artistNames(t, ", "); !a.empty()) out += "      <creator>" + xmlEscape(a) + "</creator>\n";
        if (!t.album.name.empty()) out += "      <album>" + xmlEscape(t.album.name) + "</album>\n";
        if (t.durationMs > 0) out += "      <duration>" + std::to_string(t.durationMs) + "</duration>\n";
        out += "    </track>\n";
    }
    out += "  </trackList>\n</playlist>\n";
    return out;
}

std::string writeJson(const std::string& name, const std::vector<Row>& rows) {
    json tracks = json::array();
    for (const auto& r : rows) {
        const Track& t = r.track;
        json artists = json::array();
        for (const auto& a : t.artists) artists.push_back({{"id", a.id}, {"name", a.name}});
        json images = json::array();
        for (const auto& i : t.album.images) images.push_back({{"u", i.url}, {"w", i.width}, {"h", i.height}});
        json o{{"id", t.id}, {"name", t.name}, {"artists", std::move(artists)},
               {"album", {{"id", t.album.id}, {"name", t.album.name}, {"images", std::move(images)}}},
               {"durationMs", t.durationMs}};
        if (t.explicitContent) o["explicit"] = true;
        if (t.trackNumber > 0) o["trackNumber"] = t.trackNumber;
        if (t.addedAt > 0) o["addedAt"] = t.addedAt;
        if (const std::string v = videoOf(r); !v.empty()) o["videoId"] = v;
        if (!r.filePath.empty()) o["file"] = r.filePath;
        tracks.push_back(std::move(o));
    }
    const json doc{{"format", "shadetube.playlist"}, {"version", 1}, {"name", name}, {"tracks", std::move(tracks)}};
    return doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

std::string write(Format f, const std::string& name, const std::vector<Row>& rows) {
    switch (f) {
    case Format::M3U: return writeM3u8(name, rows);
    case Format::CSV: return writeCsv(rows);
    case Format::XSPF: return writeXspf(name, rows);
    case Format::JSON: return writeJson(name, rows);
    }
    return {};
}

// ---- Reading --------------------------------------------------------------------------------------------------------

Parsed parse(std::string_view text, Format f, const fs::path& baseDir) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    switch (f) {
    case Format::M3U: return parseM3u(text, baseDir);
    case Format::CSV: return parseCsv(text, baseDir);
    case Format::XSPF: return parseXspf(text);
    case Format::JSON: return parseJson(text);
    }
    return {};
}

Parsed parseFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + toUtf8(file.wstring()));
    std::string bytes;
    in.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(std::max<std::streamoff>(0, in.tellg()));
    if (size > kMaxFileBytes) throw std::runtime_error("file too large");
    bytes.resize(size);
    in.seekg(0);
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    const std::string text = decodeText(bytes);
    const Format f = formatFromPath(file).value_or(sniff(text));
    Parsed p = parse(text, f, file.parent_path());
    if (p.name.empty()) p.name = toUtf8(file.stem().wstring());
    return p;
}

// ---- Helpers --------------------------------------------------------------------------------------------------------

std::string decodeText(std::string_view b) {
    auto u = [&](size_t i) { return static_cast<unsigned char>(b[i]); };
    if (b.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF) return std::string(b.substr(3));
    if (b.size() >= 2 && ((u(0) == 0xFF && u(1) == 0xFE) || (u(0) == 0xFE && u(1) == 0xFF))) {
        const bool be = u(0) == 0xFE;
        std::wstring w;
        w.reserve(b.size() / 2);
        for (size_t i = 2; i + 1 < b.size(); i += 2) w += static_cast<wchar_t>(be ? (u(i) << 8 | u(i + 1)) : (u(i + 1) << 8 | u(i)));
        return toUtf8(w);
    }
    if (b.empty()) return {};
    // Valid UTF-8 stays as it is; anything else is the ANSI code page of the PC (old M3U files).
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data(), static_cast<int>(b.size()), nullptr, 0) > 0)
        return std::string(b);
    const int n = MultiByteToWideChar(CP_ACP, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, b.data(), static_cast<int>(b.size()), w.data(), n);
    return toUtf8(w);
}

std::vector<std::vector<std::string>> readCsv(std::string_view text, char delimiter, char* detected) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    if (delimiter == 0) {
        // The separator the first record uses most (outside quotes): comma, semicolon (European Excel) or tab.
        int comma = 0, semi = 0, tab = 0;
        bool q = false;
        for (const char c : text) {
            if (c == '"') q = !q;
            else if (!q && (c == '\n' || c == '\r')) break;
            else if (!q && c == ',') ++comma;
            else if (!q && c == ';') ++semi;
            else if (!q && c == '\t') ++tab;
        }
        delimiter = tab > comma && tab >= semi ? '\t' : semi > comma ? ';' : ',';
    }
    if (detected) *detected = delimiter;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    auto endRow = [&] {
        row.push_back(std::move(field));
        field.clear();
        const bool blank = row.size() == 1 && row[0].empty();
        if (!blank) rows.push_back(std::move(row));
        row.clear();
        any = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = true;
            any = true;
        } else if (c == delimiter) {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            endRow();
        } else {
            field += c;
            any = true;
        }
    }
    if (any || !field.empty() || !row.empty()) endRow();
    return rows;
}

std::string importId(std::string_view name, std::string_view artist) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<uint64_t>(std::tolower(static_cast<unsigned char>(c)));
            h *= 1099511628211ull;
        }
    };
    mix(name);
    mix("|");
    mix(artist);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return std::string("import:") + buf;
}

int parseDurationMs(std::string_view text, bool millis) {
    const std::string s = trim(text);
    if (s.empty()) return 0;
    if (s.find(':') != std::string::npos) {
        double total = 0;
        size_t start = 0;
        int parts = 0;
        while (start <= s.size() && parts < 3) {
            size_t end = s.find(':', start);
            if (end == std::string::npos) end = s.size();
            std::string part = s.substr(start, end - start);
            std::replace(part.begin(), part.end(), ',', '.');
            char* stop = nullptr;
            const double v = std::strtod(part.c_str(), &stop);
            if (part.empty() || (stop && *stop)) return 0;
            total = total * 60 + v;
            ++parts;
            start = end + 1;
            if (end == s.size()) break;
        }
        return static_cast<int>(total * 1000 + 0.5);
    }
    std::string num = s;
    std::replace(num.begin(), num.end(), ',', '.');
    char* stop = nullptr;
    const double v = std::strtod(num.c_str(), &stop);
    if (stop && *stop) return 0;
    if (v <= 0) return 0;
    // A plain number: milliseconds when the column says so; else seconds, unless it is too long to be seconds of a
    // song (a "duration" column in milliseconds).
    if (millis || v > 7200) return static_cast<int>(v + 0.5);
    return static_cast<int>(v * 1000 + 0.5);
}

int64_t parseIsoTime(std::string_view text) {
    const std::string s = trim(text);
    std::tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    const int n = std::sscanf(s.c_str(), "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
    if (n < 3 || y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = n >= 4 ? h : 0;
    tm.tm_min = n >= 5 ? mi : 0;
    tm.tm_sec = n >= 6 ? sec : 0;
    int64_t t = _mkgmtime(&tm);
    if (t < 0) return 0;
    // A zone offset after the time ("+03:00", "-0500"); "Z" or none = UTC.
    const size_t tpos = s.find_first_of("T ");
    if (tpos != std::string::npos) {
        const size_t z = s.find_first_of("+-", tpos);
        if (z != std::string::npos) {
            int oh = 0, om = 0;
            if (std::sscanf(s.c_str() + z + 1, "%2d:%2d", &oh, &om) >= 1 || std::sscanf(s.c_str() + z + 1, "%2d%2d", &oh, &om) >= 1) {
                const int64_t off = (oh * 60 + om) * 60;
                t += s[z] == '+' ? -off : off;
            }
        }
    }
    return t;
}

} // namespace st::app::playlistio
