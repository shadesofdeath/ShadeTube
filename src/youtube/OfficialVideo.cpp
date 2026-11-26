#include "youtube/OfficialVideo.h"

#include "core/Http.h"
#include "core/Log.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Search/SearchClient.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace st::youtube {

using json = nlohmann::json;

namespace {

// Lower-case ASCII, other ASCII punctuation as spaces, runs of spaces collapsed; UTF-8 letters kept as they are.
std::string normalize(std::string_view s) {
    std::string out;
    bool space = true;
    for (unsigned char c : s) {
        const bool word = c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (word) {
            out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
            space = false;
        } else if (!space) {
            out += ' ';
            space = true;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string compact(std::string_view s) {
    std::string n = normalize(s);
    std::erase(n, ' ');
    return n;
}

bool hasPhrase(const std::string& text, const std::string& phrase) {
    return (" " + text + " ").find(" " + phrase + " ") != std::string::npos;
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    const std::string n = normalize(s);
    size_t i = 0;
    while (i < n.size()) {
        const size_t j = std::min(n.find(' ', i), n.size());
        if (j > i) out.push_back(n.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// "Blinding Lights (Remastered) - Radio Edit feat. X" -> "Blinding Lights".
std::string coreTitle(const std::string& title) {
    std::string out;
    int depth = 0;
    for (char c : title) {
        if (c == '(' || c == '[') {
            ++depth;
            continue;
        }
        if (c == ')' || c == ']') {
            if (depth > 0) --depth;
            continue;
        }
        if (depth == 0) out += c;
    }
    if (const auto dash = out.find(" - "); dash != std::string::npos && dash > 0) out.resize(dash);
    std::string lower = out;
    for (auto& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (const char* marker : {" feat. ", " feat ", " ft. ", " ft ", " featuring "}) {
        if (const auto p = lower.find(marker); p != std::string::npos) {
            out.resize(p);
            break;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? title : out;
}

// Uploads that are not the music video of the studio recording: refused unless the catalog title says so itself.
const char* const kRefused[] = {
    "live",        "live at",     "live from",   "concert",      "tour",         "cover",        "karaoke",     "instrumental",
    "remix",       "sped up",     "speed up",    "slowed",       "reverb",       "nightcore",    "8d",          "acoustic",
    "unplugged",   "reaction",    "reacts",      "tutorial",     "lesson",       "lyric",        "lyrics",      "letra",
    "letras",      "paroles",     "testo",       "traducao",     "tradução",     "traduction",   "traducida",   "legendado",
    "subtitulado", "vietsub",     "türkçe",      "çeviri",       "sözleri",      "visualizer",   "visualiser",  "audio",
    "1 hour",      "10 hours",    "loop",        "extended",     "mashup",       "medley",       "megamix",     "teaser",
    "trailer",     "behind the scenes", "making of", "shorts",   "fan made",     "fanmade",      "parody",      "piano",
    "guitar",      "drum",        "bass boosted", "full album",  "dance practice", "choreography", "color coded", "rehearsal",
    "session",     "sessions",    "performance", "remastered audio", "8d audio", "vertical video", "spotify",   "fan video",
};

int parseDuration(const std::string& s) {
    int total = 0, part = 0, parts = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') part = part * 10 + (c - '0');
        else if (c == ':') {
            total = total * 60 + part;
            part = 0;
            ++parts;
        } else return 0;
    }
    return parts >= 1 ? total * 60 + part : 0;
}

std::string runsText(const json& runs) {
    std::string s;
    if (runs.is_array())
        for (const auto& r : runs) s += r.value("text", "");
    return s;
}

void collectItems(const json& o, std::vector<const json*>& out) {
    if (o.is_object()) {
        if (auto it = o.find("musicResponsiveListItemRenderer"); it != o.end() && it->is_object()) {
            out.push_back(&*it);
            return;
        }
        for (const auto& [k, v] : o.items()) collectItems(v, out);
    } else if (o.is_array()) {
        for (const auto& v : o) collectItems(v, out);
    }
}

std::string findString(const json& o, const char* key) {
    if (o.is_object()) {
        if (auto it = o.find(key); it != o.end() && it->is_string()) return it->get<std::string>();
        for (const auto& [k, v] : o.items())
            if (auto s = findString(v, key); !s.empty()) return s;
    } else if (o.is_array()) {
        for (const auto& v : o)
            if (auto s = findString(v, key); !s.empty()) return s;
    }
    return {};
}

} // namespace

std::vector<OfficialCandidate> parseMusicVideoSearch(std::string_view body) {
    std::vector<OfficialCandidate> out;
    const json j = json::parse(body.begin(), body.end(), nullptr, false);
    if (j.is_discarded()) return out;
    std::vector<const json*> items;
    collectItems(j, items);
    for (const json* it : items) {
        OfficialCandidate c;
        if (auto p = it->find("playlistItemData"); p != it->end() && p->is_object()) c.videoId = p->value("videoId", "");
        if (c.videoId.empty()) c.videoId = findString(*it, "videoId");
        if (c.videoId.size() != 11) continue;
        const auto cols = it->find("flexColumns");
        if (cols == it->end() || !cols->is_array() || cols->empty()) continue;
        auto colRuns = [&](size_t i) -> const json& {
            static const json none = json::array();
            if (i >= cols->size()) return none;
            const auto& col = (*cols)[i];
            const auto r = col.find("musicResponsiveListItemFlexColumnRenderer");
            if (r == col.end()) return none;
            const auto t = r->find("text");
            if (t == r->end()) return none;
            const auto runs = t->find("runs");
            return runs == t->end() ? none : *runs;
        };
        c.title = runsText(colRuns(0));
        // Second line: artist runs (they link to a channel), " • ", views, " • ", duration.
        for (const auto& r : colRuns(1)) {
            const std::string text = r.value("text", "");
            if (r.contains("navigationEndpoint") && r["navigationEndpoint"].contains("browseEndpoint")) {
                c.artists.push_back(text);
                continue;
            }
            if (const int d = parseDuration(text); d > 0) c.durationSec = d;
        }
        if (!c.artists.empty()) c.channel = c.artists.front();
        const std::string type = findString(*it, "musicVideoType");
        c.omv = type == "MUSIC_VIDEO_TYPE_OMV";
        c.ugc = type == "MUSIC_VIDEO_TYPE_UGC";
        // A thumbnail around 400 px wide (the picker shows 96 x 54).
        if (auto th = it->find("thumbnail"); th != it->end()) {
            int bestDiff = 1 << 30;
            std::vector<const json*> lists;
            std::function<void(const json&)> find = [&](const json& o) {
                if (o.is_object()) {
                    if (auto t = o.find("thumbnails"); t != o.end() && t->is_array()) lists.push_back(&*t);
                    for (const auto& [k, v] : o.items()) find(v);
                } else if (o.is_array()) {
                    for (const auto& v : o) find(v);
                }
            };
            find(*th);
            for (const json* l : lists)
                for (const auto& t : *l) {
                    const int d = std::abs(t.value("width", 0) - 400);
                    if (t.contains("url") && d < bestDiff) {
                        bestDiff = d;
                        c.thumbnailUrl = t.value("url", "");
                    }
                }
        }
        if (c.thumbnailUrl.empty()) c.thumbnailUrl = "https://i.ytimg.com/vi/" + c.videoId + "/mqdefault.jpg";
        if (!c.title.empty()) out.push_back(std::move(c));
    }
    return out;
}

std::vector<OfficialCandidate> rankOfficialVideos(const OfficialQuery& q, std::vector<OfficialCandidate> candidates) {
    const std::string core = coreTitle(q.title);
    const auto coreWords = words(core);
    const std::string catalog = normalize(q.title + " " + q.album);
    const std::string artist = q.artists.empty() ? std::string() : q.artists.front();
    const auto artistWords = words(artist);
    const std::string artistCompact = compact(artist);
    std::vector<OfficialCandidate> out;
    std::unordered_set<std::string> seen;
    int index = 0;
    for (auto& c : candidates) {
        const int rank = index++;
        if (!seen.insert(c.videoId).second) continue;
        const std::string title = normalize(c.title);
        const std::string channel = normalize(c.channel);
        std::string artistsText;
        for (const auto& a : c.artists) artistsText += " " + normalize(a);
        const std::string where = title + " " + channel + artistsText;
        // The song: its title's words in the video's title.
        if (!coreWords.empty()) {
            size_t hit = 0;
            for (const auto& w : coreWords) hit += hasPhrase(title, w) ? 1 : 0;
            if (static_cast<double>(hit) / static_cast<double>(coreWords.size()) < 0.8) continue;
        }
        // The artist: in YouTube Music's artists, the channel or the title.
        bool artistHere = artistWords.empty();
        if (!artistWords.empty()) {
            size_t hit = 0;
            for (const auto& w : artistWords) hit += hasPhrase(where, w) ? 1 : 0;
            artistHere = hit == artistWords.size() || (!artistCompact.empty() && compact(c.channel).find(artistCompact) != std::string::npos);
        }
        if (!artistHere) continue;
        // Art Tracks and every kind of other version.
        if (normalize(c.channel).ends_with(" topic")) continue;
        bool refused = false;
        for (const char* p : kRefused) {
            const std::string phrase = normalize(p);
            if (hasPhrase(title, phrase) && !hasPhrase(catalog, phrase)) {
                refused = true;
                break;
            }
        }
        if (refused) continue;
        // Length: a music video may add an intro / outro, rarely cut much.
        if (q.durationSec > 0 && c.durationSec > 0) {
            const double song = q.durationSec, d = c.durationSec;
            if (d < song - std::max(20.0, 0.12 * song) || d > song + std::max(180.0, 0.75 * song)) continue;
        }
        // Official signals.
        double s = 0;
        const bool artistChannel = !artistCompact.empty() && compact(c.channel) == artistCompact;
        const bool vevo = compact(c.channel).find("vevo") != std::string::npos;
        const bool artistRun = std::any_of(c.artists.begin(), c.artists.end(), [&](const std::string& a) { return compact(a) == artistCompact; });
        const bool officialTitle =
            hasPhrase(title, "official video") || hasPhrase(title, "official music video") || hasPhrase(title, "music video");
        if (c.omv) s += 40;
        if (artistRun) s += 10;
        if (artistChannel) s += 20;
        else if (vevo) s += 20;
        if (officialTitle) s += 10;
        else if (hasPhrase(title, "official")) s += 3;
        if (c.ugc) s -= 15;
        if (q.durationSec > 0 && c.durationSec > 0) s -= std::min(10.0, std::fabs(c.durationSec - q.durationSec) / 30.0);
        s += std::max(0.0, 3.0 - 0.3 * rank);
        if (s < 20) continue;   // nothing says it is the artist's own video
        c.score = s;
        out.push_back(std::move(c));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (out.size() > 4) out.resize(4);
    return out;
}

std::vector<OfficialCandidate> findOfficialVideos(YoutubeExplode::YoutubeClient& client, const OfficialQuery& q,
                                                  const std::vector<std::string>& exclude, const std::string& region,
                                                  const YoutubeExplode::CancellationToken& ct) {
    const std::string artist = q.artists.empty() ? std::string() : q.artists.front();
    const std::string core = coreTitle(q.title);
    auto drop = [&](std::vector<OfficialCandidate>& v) {
        std::erase_if(v, [&](const OfficialCandidate& c) { return std::find(exclude.begin(), exclude.end(), c.videoId) != exclude.end(); });
    };
    std::vector<OfficialCandidate> ranked;
    // 1. YouTube Music's video search: its OMV label is YouTube Music's own "this is the official video".
    try {
        json body = {{"context",
                      {{"client", {{"clientName", "WEB_REMIX"}, {"clientVersion", "1.20261118.01.00"}, {"hl", "en"}, {"gl", region.empty() ? "US" : region}}}}},
                     {"query", artist.empty() ? core : artist + " " + core},
                     {"params", "EgWKAQIQAWoKEAkQChAFEAMQBA%3D%3D"}};
        http::HttpRequest req;
        req.method = "POST";
        req.url = "https://music.youtube.com/youtubei/v1/search?prettyPrint=false";
        req.headers = {{"Content-Type", "application/json"}, {"Origin", "https://music.youtube.com"}, {"Referer", "https://music.youtube.com/"}};
        req.body = body.dump();
        const auto resp = http::send(std::move(req), ct);
        if (resp.isSuccessStatusCode()) {
            ranked = rankOfficialVideos(q, parseMusicVideoSearch(resp.body));
            drop(ranked);
        } else {
            ST_LOG_WARN("video", "YouTube Music video search: HTTP {}", resp.statusCode);
        }
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        ST_LOG_WARN("video", "YouTube Music video search failed: {}", e.what());
    }
    // 2. YouTube's own search when that found nothing usable.
    if (ranked.empty()) {
        std::vector<OfficialCandidate> found;
        for (const auto& v : client.search().getVideos((artist.empty() ? core : artist + " - " + core) + " official video", 15, ct)) {
            OfficialCandidate c;
            c.videoId = v.id().value();
            c.title = v.title();
            c.channel = v.author().channelTitle();
            if (v.duration()) c.durationSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(*v.duration()).count());
            for (const auto& th : v.thumbnails())
                if (c.thumbnailUrl.empty() || (th.resolution().width() >= 160 && th.resolution().width() <= 480)) c.thumbnailUrl = th.url();
            found.push_back(std::move(c));
        }
        ranked = rankOfficialVideos(q, std::move(found));
        drop(ranked);
    }
    return ranked;
}

} // namespace st::youtube
