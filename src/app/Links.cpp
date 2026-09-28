#include "app/Links.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace st::app::links {
namespace {

constexpr std::string_view kSpace = " \t\r\n\f\v";

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}
bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != lower(prefix[i])) return false;
    return true;
}
bool isAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string_view trim(std::string_view s) {
    const size_t b = s.find_first_not_of(kSpace);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(kSpace) - b + 1);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(sep, start);
        const auto part = s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!part.empty()) out.push_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

Link make(Kind kind, Service service, std::string id = {}) {
    Link l;
    l.kind = kind;
    l.service = service;
    l.id = std::move(id);
    return l;
}
Link unsupported(Service service) { return make(Kind::Unsupported, service); }

Kind spotifyKind(std::string_view type) {
    if (type == "track") return Kind::SpotifyTrack;
    if (type == "album") return Kind::SpotifyAlbum;
    if (type == "playlist") return Kind::SpotifyPlaylist;
    if (type == "artist") return Kind::SpotifyArtist;
    return Kind::None;
}

Link spotifyItem(std::string_view type, std::string_view id) {
    const Kind k = spotifyKind(toLower(type));
    if (k == Kind::None || !isSpotifyId(id)) return unsupported(Service::Spotify);
    return make(k, Service::Spotify, std::string(id));
}

// spotify:track:<id>, spotify:user:<name>:playlist:<id>
Link parseSpotifyUri(std::string_view s) {
    const auto parts = split(s.substr(8), ':');
    if (parts.size() == 2) return spotifyItem(parts[0], parts[1]);
    if (parts.size() == 4 && toLower(parts[0]) == "user" && toLower(parts[2]) == "playlist")
        return spotifyItem(parts[2], parts[3]);
    return unsupported(Service::Spotify);
}

// open.spotify.com path segments: [intl-xx/][embed/]<type>/<id>, or user/<name>/playlist/<id>.
Link parseSpotifyPath(std::vector<std::string_view> seg) {
    auto dropFront = [&](auto pred) {
        if (!seg.empty() && pred(toLower(seg.front()))) seg.erase(seg.begin());
    };
    dropFront([](const std::string& s) { return s.rfind("intl-", 0) == 0 && s.size() >= 7 && s.size() <= 10; });
    dropFront([](const std::string& s) { return s == "embed" || s == "embed-podcast"; });
    if (seg.size() >= 2 && spotifyKind(toLower(seg[0])) != Kind::None) return spotifyItem(seg[0], seg[1]);
    if (seg.size() >= 4 && toLower(seg[0]) == "user" && toLower(seg[2]) == "playlist") return spotifyItem(seg[2], seg[3]);
    return unsupported(Service::Spotify);
}

std::string_view queryValue(std::string_view query, std::string_view key) {
    for (const auto part : split(query, '&')) {
        const size_t eq = part.find('=');
        if (eq != std::string_view::npos && part.substr(0, eq) == key) return part.substr(eq + 1);
    }
    return {};
}

Link video(std::string_view id) {
    if (!isVideoId(id)) return unsupported(Service::YouTube);
    return make(Kind::YouTubeVideo, Service::YouTube, std::string(id));
}

Link parseYouTube(const std::vector<std::string_view>& seg, std::string_view query) {
    if (seg.empty()) return unsupported(Service::YouTube);
    const std::string first = toLower(seg[0]);
    if (first == "watch") return video(queryValue(query, "v"));
    if ((first == "shorts" || first == "embed" || first == "live" || first == "v" || first == "e") && seg.size() >= 2)
        return video(seg[1]);
    return unsupported(Service::YouTube);   // playlists, channels, @handles, search results...
}

Link parseMusicBrainz(const std::vector<std::string_view>& seg) {
    if (seg.size() < 2 || !isMbid(seg[1])) return unsupported(Service::MusicBrainz);
    const std::string type = toLower(seg[0]);
    const std::string id = toLower(seg[1]);
    if (type == "release-group") return make(Kind::MbReleaseGroup, Service::MusicBrainz, id);
    if (type == "release") return make(Kind::MbRelease, Service::MusicBrainz, id);
    if (type == "artist") return make(Kind::MbArtist, Service::MusicBrainz, id);
    if (type == "recording") return make(Kind::MbRecording, Service::MusicBrainz, id);
    return unsupported(Service::MusicBrainz);
}

} // namespace

std::string Link::spotifyUri() const {
    switch (kind) {
    case Kind::SpotifyTrack: return "spotify:track:" + id;
    case Kind::SpotifyAlbum: return "spotify:album:" + id;
    case Kind::SpotifyPlaylist: return "spotify:playlist:" + id;
    case Kind::SpotifyArtist: return "spotify:artist:" + id;
    default: return {};
    }
}

bool isSpotifyId(std::string_view s) { return s.size() == 22 && std::all_of(s.begin(), s.end(), isAlnum); }

bool isVideoId(std::string_view s) {
    return s.size() == 11 && std::all_of(s.begin(), s.end(), [](char c) { return isAlnum(c) || c == '_' || c == '-'; });
}

bool isMbid(std::string_view s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !isHex(s[i])) return false;
    }
    return true;
}

Link parse(std::string_view text) {
    std::string_view s = trim(text);
    // Pasted between <...> or quotes (chat apps, Markdown).
    while (s.size() >= 2 && ((s.front() == '<' && s.back() == '>') || (s.front() == '"' && s.back() == '"') ||
                             (s.front() == '\'' && s.back() == '\''))) {
        s = trim(s.substr(1, s.size() - 2));
    }
    if (s.empty() || s.find_first_of(kSpace) != std::string_view::npos) return {};
    if (startsWithNoCase(s, "spotify:")) return parseSpotifyUri(s);

    if (startsWithNoCase(s, "https://")) s.remove_prefix(8);
    else if (startsWithNoCase(s, "http://")) s.remove_prefix(7);
    const size_t hostEnd = s.find_first_of("/?#");
    std::string_view authority = s.substr(0, hostEnd);
    if (const size_t at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (const size_t colon = authority.find(':'); colon != std::string_view::npos) authority = authority.substr(0, colon);
    std::string host = toLower(authority);
    if (host.rfind("www.", 0) == 0) host.erase(0, 4);
    if (host.find('.') == std::string::npos) return {};

    std::string_view rest = hostEnd == std::string_view::npos ? std::string_view{} : s.substr(hostEnd);
    if (const size_t hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
    std::string_view path = rest, query;
    if (const size_t q = rest.find('?'); q != std::string_view::npos) {
        path = rest.substr(0, q);
        query = rest.substr(q + 1);
    }
    const auto seg = split(path, '/');

    if (host == "open.spotify.com" || host == "play.spotify.com") return parseSpotifyPath(seg);
    if (host == "spotify.link" || host == "spotify.app.link") return unsupported(Service::Spotify);   // short links
    if (host == "youtu.be") return seg.empty() ? unsupported(Service::YouTube) : video(seg[0]);
    if (host == "youtube.com" || host == "m.youtube.com" || host == "music.youtube.com" ||
        host == "youtube-nocookie.com")
        return parseYouTube(seg, query);
    if (host == "musicbrainz.org" || host == "beta.musicbrainz.org") return parseMusicBrainz(seg);
    return {};
}

Link parse(std::wstring_view text) {
    // Every link we recognize is ASCII: anything else only has to stay "not ASCII".
    std::string narrow;
    narrow.reserve(text.size());
    for (wchar_t c : text) narrow.push_back(c < 0x80 ? static_cast<char>(c) : '\x7f');
    return parse(narrow);
}

// ---- YouTube video -> song ---------------------------------------------------------------------------------------

namespace {

// Words of a bracket group, ASCII lower-cased ("Official Music Video" -> official, music, video). UTF-8 sequences
// stay inside their word ("Sözleri" -> "sözleri").
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (isAlnum(c) || static_cast<unsigned char>(c) >= 0x80) cur.push_back(lower(c));
        else if (!cur.empty()) out.push_back(std::exchange(cur, {}));
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// A bracket group that only says what kind of upload this is.
bool noiseGroup(std::string_view inner) {
    static constexpr std::array<std::string_view, 12> keep = {"feat", "ft", "featuring", "live", "remix", "mix",
                                                              "edit", "version", "acoustic", "cover", "remaster",
                                                              "remastered"};
    static constexpr std::array<std::string_view, 17> noise = {
        "official", "video", "audio", "lyric", "lyrics", "visualizer", "visualiser", "klip", "clip",
        "videoclip", "videoklip", "mv", "hd", "hq", "4k", "1080p", "sözleri"};
    bool isNoise = false;
    for (const auto& w : words(inner)) {
        if (std::find(keep.begin(), keep.end(), w) != keep.end()) return false;
        if (std::find(noise.begin(), noise.end(), w) != noise.end()) isNoise = true;
    }
    // "(Official Live Video)" stays (live); "[4K Remaster]" stays too: a remaster is a different recording.
    return isNoise;
}

std::string collapse(std::string_view s) {
    std::string out;
    for (char c : s) {
        if ((c == ' ' || c == '\t') && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c == '\t' ? ' ' : c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Drops noise groups: (...), [...] and the CJK 【...】.
std::string stripNoise(std::string_view title) {
    struct Pair {
        std::string_view open, close;
    };
    static constexpr std::array<Pair, 3> pairs = {{{"(", ")"}, {"[", "]"}, {"\xE3\x80\x90", "\xE3\x80\x91"}}};
    std::string s(title);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& p : pairs) {
            size_t from = 0;
            while (true) {
                const size_t a = s.find(p.open, from);
                if (a == std::string::npos) break;
                const size_t b = s.find(p.close, a + p.open.size());
                if (b == std::string::npos) break;
                const std::string_view inner(s.data() + a + p.open.size(), b - a - p.open.size());
                if (noiseGroup(inner)) {
                    s.erase(a, b + p.close.size() - a);
                    changed = true;
                } else {
                    from = b + p.close.size();
                }
            }
        }
    }
    return collapse(s);
}

std::string cleanChannel(std::string_view channel) {
    std::string c = collapse(trim(channel));
    for (std::string_view suffix : {std::string_view(" - Topic"), std::string_view("VEVO"), std::string_view(" Official")}) {
        if (c.size() > suffix.size() + 1 && c.compare(c.size() - suffix.size(), suffix.size(), suffix) == 0) {
            c.erase(c.size() - suffix.size());
            c = collapse(c);
        }
    }
    return c;
}

} // namespace

VideoSong videoSong(std::string_view title, std::string_view channel) {
    VideoSong out;
    const bool topic = channel.size() > 8 && channel.substr(channel.size() - 8) == " - Topic";
    std::string t = stripNoise(trim(title));
    if (t.empty()) t = collapse(trim(title));
    out.artist = cleanChannel(channel);
    if (!topic) {
        // "Artist - Title" (hyphen, en or em dash between spaces)
        for (std::string_view sep : {std::string_view(" - "), std::string_view(" \xE2\x80\x93 "), std::string_view(" \xE2\x80\x94 ")}) {
            const size_t p = t.find(sep);
            if (p == std::string::npos || p == 0 || p + sep.size() >= t.size()) continue;
            std::string artist = collapse(trim(std::string_view(t).substr(0, p)));
            std::string song = collapse(trim(std::string_view(t).substr(p + sep.size())));
            if (artist.empty() || song.empty()) continue;
            out.artist = std::move(artist);
            t = std::move(song);
            break;
        }
    }
    // Leftover quotes around the song ("Artist - 'Title'").
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\'')))
        t = collapse(trim(std::string_view(t).substr(1, t.size() - 2)));
    out.title = std::move(t);
    return out;
}

} // namespace st::app::links
