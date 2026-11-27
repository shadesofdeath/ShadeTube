#include "app/Recognition.h"

#include "app/CatalogJson.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "musicbrainz/Parse.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <fstream>

namespace st::app::recognition {

namespace {

using nlohmann::json;

std::string lettersAndDigits(std::string_view text) {
    const std::wstring folded = foldForSearch(toWide(text));
    std::wstring out;
    for (wchar_t c : folded)
        if (std::iswalnum(c)) out += c;
    return toUtf8(out);
}

// A song known by name: "import:<FNV-1a of name|artist>", the form app/HistoryImport gives such songs (the listening
// stats merge every "import:" id by its name and artist).
std::string nameId(std::string_view name, std::string_view artist) {
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

bool sameSong(const recognize::Song& a, const recognize::Song& b) {
    return foldTitle(a.title) == foldTitle(b.title) && foldArtist(a.artist) == foldArtist(b.artist);
}

json songJson(const recognize::Song& s) {
    return {{"key", s.key},     {"title", s.title},     {"artist", s.artist}, {"album", s.album},
            {"label", s.label}, {"year", s.year},       {"genre", s.genre},   {"isrc", s.isrc},
            {"cover", s.coverUrl}, {"apple", s.appleMusicUrl}, {"shazam", s.shazamUrl}};
}

recognize::Song songFromJson(const json& j) {
    recognize::Song s;
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    s.key = str("key");
    s.title = str("title");
    s.artist = str("artist");
    s.album = str("album");
    s.label = str("label");
    s.year = str("year");
    s.genre = str("genre");
    s.isrc = str("isrc");
    s.coverUrl = str("cover");
    s.appleMusicUrl = str("apple");
    s.shazamUrl = str("shazam");
    return s;
}

} // namespace

std::string foldTitle(std::string_view title) {
    const std::string clean = mb::detail::searchableTitle(title);
    const std::string folded = lettersAndDigits(clean);
    return folded.empty() ? lettersAndDigits(title) : folded;   // a title that is only "(...)" keeps it
}

std::string foldArtist(std::string_view artist) { return lettersAndDigits(artist); }

std::vector<std::string> splitArtists(std::string_view credit) {
    std::vector<std::string> out;
    std::string rest(credit);
    // Separators between credited names, longest first; matched case-insensitively with spaces around the words.
    static const char* kSeparators[] = {" featuring ", " feat. ", " feat ", " ft. ", " ft ", " with ", " & ", " x ", " X ",
                                        " and ", ", ", " / ", " + ", " vs. ", " vs "};
    std::string lower = rest;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    size_t start = 0;
    while (start < rest.size()) {
        size_t best = std::string::npos, len = 0;
        for (const char* sep : kSeparators) {
            std::string s(sep);
            std::transform(s.begin(), s.end(), s.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            const size_t at = lower.find(s, start);
            if (at != std::string::npos && (at < best || (at == best && s.size() > len))) {
                best = at;
                len = s.size();
            }
        }
        std::string part = rest.substr(start, best == std::string::npos ? std::string::npos : best - start);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) out.push_back(part);
        if (best == std::string::npos) break;
        start = best + len;
    }
    if (out.empty() && !credit.empty()) out.emplace_back(credit);
    return out;
}

int bestMatch(const std::vector<catalog::Track>& candidates, const recognize::Song& song) {
    const std::string title = foldTitle(song.title);
    if (title.empty()) return -1;
    std::vector<std::string> artists;
    for (const auto& a : splitArtists(song.artist))
        if (auto f = foldArtist(a); !f.empty()) artists.push_back(std::move(f));
    const std::string whole = foldArtist(song.artist);
    auto artistFits = [&](const catalog::Track& t) {
        if (artists.empty()) return true;
        for (const auto& ref : t.artists) {
            const std::string name = foldArtist(ref.name);
            if (name.empty()) continue;
            if (name == whole) return true;
            for (const auto& a : artists)
                if (a == name || (a.size() >= 4 && name.find(a) != std::string::npos) ||
                    (name.size() >= 4 && a.find(name) != std::string::npos))
                    return true;
        }
        return false;
    };
    int fallback = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& t = candidates[i];
        if (!t.playable || t.name.empty() || !artistFits(t)) continue;
        const std::string name = foldTitle(t.name);
        if (name == title) return static_cast<int>(i);
        // "Song" against "Song (Live at ...)" / "Song Pt. 1": accepted only when nothing fits exactly.
        if (fallback < 0 && title.size() >= 4 && name.size() >= 4 &&
            (name.rfind(title, 0) == 0 || title.rfind(name, 0) == 0))
            fallback = static_cast<int>(i);
    }
    return fallback;
}

std::string searchQuery(const recognize::Song& song) {
    const auto artists = splitArtists(song.artist);
    const std::string artist = artists.empty() ? song.artist : artists.front();
    const std::string title = mb::detail::searchableTitle(song.title);
    return artist.empty() ? title : artist + " " + (title.empty() ? song.title : title);
}

catalog::Track fallbackTrack(const recognize::Song& song) {
    catalog::Track t;
    t.name = song.title;
    for (const auto& a : splitArtists(song.artist)) t.artists.push_back({"", a});
    if (t.artists.empty() && !song.artist.empty()) t.artists.push_back({"", song.artist});
    t.album.name = song.album;
    if (!song.coverUrl.empty()) t.album.images.push_back({song.coverUrl, 400, 400});
    t.id = nameId(t.name, t.artists.empty() ? std::string() : t.artists.front().name);
    return t;
}

// ---- history ------------------------------------------------------------------------------------------------------------

void History::load(const std::filesystem::path& file) {
    file_ = file;
    entries_.clear();
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    const json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        ST_LOG_WARN("recognize", "recognized.json is unreadable: starting a new history");
        return;
    }
    if (j.value("source", std::string()) == "microphone") source_ = recognize::Source::Microphone;
    if (const auto it = j.find("songs"); it != j.end() && it->is_array())
        for (const auto& e : *it) {
            if (!e.is_object() || !e.contains("song") || !e["song"].is_object()) continue;
            Entry en;
            en.at = e.value("at", int64_t{0});
            en.song = songFromJson(e["song"]);
            if (en.song.title.empty()) continue;
            if (e.contains("track") && e["track"].is_object()) en.track = trackFromJson(e["track"]);
            entries_.push_back(std::move(en));
            if (entries_.size() >= kHistoryMax) break;
        }
}

bool History::save() const {
    if (file_.empty()) return false;
    json j;
    j["version"] = 1;
    j["source"] = source_ == recognize::Source::Microphone ? "microphone" : "system";
    json songs = json::array();
    for (const auto& e : entries_) {
        json o{{"at", e.at}, {"song", songJson(e.song)}};
        if (!e.track.id.empty()) o["track"] = toJson(e.track);
        songs.push_back(std::move(o));
    }
    j["songs"] = std::move(songs);
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const auto tmp = std::filesystem::path(file_).concat(L".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(1);
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        ST_LOG_WARN("recognize", "recognized.json not written: {}", ec.message());
        return false;
    }
    return true;
}

void History::add(Entry e) {
    std::erase_if(entries_, [&](const Entry& x) { return sameSong(x.song, e.song); });
    e.song.offsetSec = 0;
    e.song.timeSkew = e.song.frequencySkew = 0;
    entries_.insert(entries_.begin(), std::move(e));
    if (entries_.size() > kHistoryMax) entries_.resize(kHistoryMax);
}

void History::setTrack(const recognize::Song& song, const catalog::Track& track) {
    for (auto& e : entries_)
        if (sameSong(e.song, song)) e.track = track;
}

void History::clear() { entries_.clear(); }

} // namespace st::app::recognition
