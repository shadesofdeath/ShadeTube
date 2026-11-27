// Matching songs of another service to the active catalog (see CatalogMatch.h).
#include "app/CatalogMatch.h"

#include "app/PlaylistIO.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/SpotifyApi.h"
#include "ytmusic/YtMusic.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace st::app::catmatch {

namespace {

using catalog::Track;
using OperationCanceled = YoutubeExplode::Exceptions::OperationCanceledException;

constexpr const char* kTag = "catmatch";

// Words of a group that only say which edition / credit it is.
bool noiseWord(const std::wstring& w) {
    static const std::unordered_set<std::wstring> kNoise = {
        L"remaster", L"remastered", L"remasterizado", L"remasterizada", L"remasterise", L"remasterisee", L"remasterizzato",
        L"feat", L"ft", L"featuring", L"with", L"edit", L"version", L"versao", L"version", L"mono", L"stereo", L"explicit",
        L"clean", L"deluxe", L"bonus", L"anniversary", L"edition", L"single", L"album", L"original", L"from", L"soundtrack",
        L"ost", L"motion", L"picture", L"radio", L"mix", L"digital", L"remasterizada", L"digitally", L"track", L"expanded"};
    if (kNoise.count(w)) return true;
    // A year (1950-2099) or a number like "40th".
    if (w.size() == 4 && std::all_of(w.begin(), w.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return true;
    return false;
}
// Words that make it another recording (kept apart: one side having it and the other not costs).
bool flagWord(const std::wstring& w) {
    static const std::unordered_set<std::wstring> kFlags = {L"live",       L"remix",   L"acoustic", L"instrumental", L"karaoke",
                                                            L"cover",      L"demo",    L"sped",     L"slowed",       L"nightcore",
                                                            L"unplugged",  L"acapella", L"reprise", L"extended",     L"remixed"};
    return kFlags.count(w) > 0;
}
// "and" in a few languages, articles and credit words: never what tells two artists apart. (Narrow literals: these are
// data, not UI text.)
bool stopWord(const std::wstring& w) {
    static const std::unordered_set<std::wstring> kStop = [] {
        std::unordered_set<std::wstring> s;
        for (const char* x : {"the", "and", "feat", "ft", "featuring", "with", "x", "vs", "y", "e", "et", "und", "ve"}) s.insert(toWide(x));
        return s;
    }();
    return kStop.count(w) > 0;
}

std::wstring fold(std::string_view s) {
    std::wstring w = foldForSearch(toWide(s));
    for (auto& c : w)
        if (c == L'ı' || c == L'İ') c = L'i';
    return w;
}

std::vector<std::wstring> splitWords(std::wstring_view w) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : w) {
        if (IsCharAlphaNumericW(c)) cur.push_back(c);
        else if (!cur.empty()) out.push_back(std::exchange(cur, {}));
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// What a bracketed group / " - " suffix is: noise (dropped), a flag (dropped, flags kept) or part of the title.
enum class Group { Title, Noise, Flag };
Group classify(const std::vector<std::wstring>& ws, std::vector<std::wstring>* flags) {
    bool noise = false, flag = false;
    for (const auto& w : ws) {
        if (flagWord(w)) {
            flag = true;
            if (flags) flags->push_back(w == L"remixed" ? L"remix" : w);
        } else if (noiseWord(w)) {
            noise = true;
        }
    }
    return flag ? Group::Flag : noise ? Group::Noise : Group::Title;
}

// The title with its noise groups removed (flag groups too when `dropFlags`), as text: folded (for words) or original.
// Works on wide text so brackets are found in any script.
std::wstring stripGroups(std::wstring s, bool dropFlags, std::vector<std::wstring>* flags) {
    // "feat. X" / "ft. X" outside brackets: the rest of the title is the credit.
    for (const std::wstring_view cut : {std::wstring_view(L" feat. "), std::wstring_view(L" feat "), std::wstring_view(L" ft. "),
                                        std::wstring_view(L" featuring "), std::wstring_view(L" Feat. "), std::wstring_view(L" Ft. "),
                                        std::wstring_view(L" FEAT. ")}) {
        if (const size_t p = s.find(cut); p != std::wstring::npos && p > 0) s.resize(p);
    }
    static constexpr std::pair<wchar_t, wchar_t> kPairs[] = {{L'(', L')'}, {L'[', L']'}, {L'{', L'}'}};
    for (const auto& [open, close] : kPairs) {
        size_t from = 0;
        while (true) {
            const size_t a = s.find(open, from);
            if (a == std::wstring::npos) break;
            const size_t b = s.find(close, a + 1);
            if (b == std::wstring::npos) break;
            const auto ws = splitWords(foldForSearch(std::wstring_view(s).substr(a + 1, b - a - 1)));
            const Group g = classify(ws, flags);
            if (g == Group::Noise || (g == Group::Flag && dropFlags)) {
                s.erase(a, b + 1 - a);
                from = a;
            } else {
                from = b + 1;
            }
        }
    }
    // " - Remastered 2011", " - Live at Wembley", " - Radio Edit" (Spotify's style): the suffix goes when it is noise.
    for (const std::wstring_view dash : {std::wstring_view(L" - "), std::wstring_view(L" – "), std::wstring_view(L" — ")}) {
        const size_t p = s.find(dash);
        if (p == std::wstring::npos || p == 0) continue;
        const auto ws = splitWords(foldForSearch(std::wstring_view(s).substr(p + dash.size())));
        const Group g = classify(ws, flags);
        if (g == Group::Noise || (g == Group::Flag && dropFlags)) s.resize(p);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'-')) s.pop_back();
    return s;
}

double dice(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    if (a.empty() || b.empty()) return 0;
    std::unordered_set<std::wstring> sa(a.begin(), a.end()), sb(b.begin(), b.end());
    size_t common = 0;
    for (const auto& w : sa) common += sb.count(w);
    return 2.0 * static_cast<double>(common) / static_cast<double>(sa.size() + sb.size());
}

// Share of the smaller set's words found in the other.
double containment(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    std::unordered_set<std::wstring> sa(a.begin(), a.end()), sb(b.begin(), b.end());
    if (sa.empty() || sb.empty()) return 0;
    const auto& small = sa.size() <= sb.size() ? sa : sb;
    const auto& large = sa.size() <= sb.size() ? sb : sa;
    size_t common = 0;
    for (const auto& w : small) common += large.count(w);
    return static_cast<double>(common) / static_cast<double>(small.size());
}

std::vector<std::wstring> nameWords(std::string_view name) {
    auto ws = words(name);
    std::erase_if(ws, stopWord);
    return ws;
}

double titleSimilarity(std::string_view a, std::string_view b, std::vector<std::wstring>* flagsA, std::vector<std::wstring>* flagsB) {
    auto ca = coreWords(a, flagsA), cb = coreWords(b, flagsB);
    if (ca.empty() || cb.empty()) {   // a title of noise only: compare everything
        ca = words(a);
        cb = words(b);
    }
    if (ca == cb) return 1;
    return dice(ca, cb);
}

double artistSimilarity(const std::vector<std::string>& want, const std::vector<std::string>& cand) {
    double bestPair = 0;
    std::vector<std::wstring> allWant, allCand;
    for (const auto& w : want) {
        const auto ww = nameWords(w);
        allWant.insert(allWant.end(), ww.begin(), ww.end());
        for (const auto& c : cand) bestPair = std::max(bestPair, containment(ww, nameWords(c)));
    }
    for (const auto& c : cand) {
        const auto cw = nameWords(c);
        allCand.insert(allCand.end(), cw.begin(), cw.end());
    }
    return std::max(bestPair, dice(allWant, allCand));
}

double durationSimilarity(int a, int b) {
    if (a <= 0 || b <= 0) return 0.6;
    const double d = std::abs(a - b) / 1000.0;
    if (d <= 2) return 1;
    if (d <= 5) return 0.9;
    if (d <= 10) return 0.6;
    if (d <= 20) return 0.3;
    return 0;
}

std::vector<std::string> artistNames(const std::vector<catalog::ArtistRef>& refs) {
    std::vector<std::string> out;
    for (const auto& r : refs)
        if (!r.name.empty()) out.push_back(r.name);
    return out;
}

std::string mainArtist(const svc::Song& s) { return s.artists.empty() ? std::string{} : s.artists[0]; }

// The title to search for: noise groups gone, a live / remix group kept (it finds that version).
std::string searchTitle(const std::string& title) { return toUtf8(stripGroups(toWide(title), false, nullptr)); }

} // namespace

const wchar_t* catalogName(Catalog c) {
    switch (c) {
    case Catalog::Spotify: return L"Spotify";
    case Catalog::YouTubeMusic: return L"YouTube Music";
    case Catalog::MusicBrainz: return L"MusicBrainz";
    }
    return L"";
}

std::vector<std::wstring> words(std::string_view text) { return splitWords(fold(text)); }

std::vector<std::wstring> coreWords(std::string_view title, std::vector<std::wstring>* flags) {
    return splitWords(fold(toUtf8(stripGroups(toWide(title), true, flags))));
}

double score(const svc::Song& want, const Track& cand) {
    std::vector<std::wstring> fw, fc;
    const double t = titleSimilarity(want.title, cand.name, &fw, &fc);
    const auto candArtists = artistNames(cand.artists);
    const bool artistsKnown = !want.artists.empty() && !candArtists.empty();
    const double a = artistsKnown ? artistSimilarity(want.artists, candArtists) : 0.5;
    const double d = durationSimilarity(want.durationMs, cand.durationMs);
    double s = 100 * (0.5 * t + 0.3 * a + 0.2 * d);
    if (t < 0.5) s *= 0.5;
    if (artistsKnown && a < 0.34) s *= 0.6;
    std::sort(fw.begin(), fw.end());
    fw.erase(std::unique(fw.begin(), fw.end()), fw.end());
    std::sort(fc.begin(), fc.end());
    fc.erase(std::unique(fc.begin(), fc.end()), fc.end());
    if (fw != fc) s -= 35;
    return std::clamp(s, 0.0, 100.0);
}

int best(const svc::Song& want, const std::vector<Track>& candidates, double minScore, double* bestScore) {
    int bestIndex = -1;
    double top = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].id.empty()) continue;
        const double s = score(want, candidates[i]);
        if (s > top) {
            top = s;
            bestIndex = static_cast<int>(i);
        }
    }
    if (bestScore) *bestScore = std::max(0.0, top);
    return top >= minScore ? bestIndex : -1;
}

double albumScore(const std::string& title, const std::string& artist, const catalog::Album& cand) {
    const double t = titleSimilarity(title, cand.name, nullptr, nullptr);
    const auto names = artistNames(cand.artists);
    const double a = names.empty() || artist.empty() ? 0.5 : artistSimilarity({artist}, names);
    double s = 100 * (0.6 * t + 0.4 * a);
    if (t < 0.5) s *= 0.5;
    if (!names.empty() && !artist.empty() && a < 0.34) s *= 0.6;
    return std::clamp(s, 0.0, 100.0);
}

Track nameOnly(const svc::Song& s) {
    Track t;
    t.id = playlistio::importId(s.title, mainArtist(s));
    t.name = s.title;
    for (const auto& a : s.artists) t.artists.push_back({"", a});
    t.album.name = s.album;
    if (!s.coverUrl.empty()) t.album.images.push_back({s.coverUrl, 600, 600});
    t.durationMs = s.durationMs;
    t.trackNumber = s.trackNumber;
    t.addedAt = s.addedAt;
    return t;
}

// ---- Lookups --------------------------------------------------------------------------------------------------------

namespace {

// Runs `work(i)` for every index on `threads` threads; the first cancellation stops them all and is rethrown.
void parallel(size_t count, int threads, const CT& ct, const std::function<void(size_t)>& work) {
    std::atomic<size_t> next{0};
    std::atomic<bool> cancelled{false};
    auto run = [&] {
        while (!cancelled) {
            const size_t i = next++;
            if (i >= count) return;
            try {
                ct.throwIfCancellationRequested();
                work(i);
            } catch (const OperationCanceled&) {
                cancelled = true;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(run);
    run();
    for (auto& t : pool) t.join();
    ct.throwIfCancellationRequested();
    if (cancelled) throw OperationCanceled();
}

void sleepFor(std::chrono::milliseconds d, const CT& ct) {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) {
        ct.throwIfCancellationRequested();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// Failures in a row across the threads: after kGiveUp the remaining songs are left by name.
struct Health {
    static constexpr int kGiveUp = 12;
    std::atomic<int> inRow{0};
    std::atomic<bool> gaveUp{false};
    void ok() { inRow = 0; }
    void fail(const char* what, const std::string& song, const std::exception& e) {
        ST_LOG_WARN(kTag, "{} '{}': {}", what, song, e.what());
        if (++inRow >= kGiveUp && !gaveUp.exchange(true)) ST_LOG_WARN(kTag, "{} failures in a row: giving up", kGiveUp);
    }
};

void found(Result& r, const Track& t, bool byIsrc, Counters& c, const svc::Song& song) {
    r.track = t;
    if (r.track.trackNumber == 0) r.track.trackNumber = song.trackNumber;
    if (r.track.album.images.empty() && !song.coverUrl.empty()) r.track.album.images.push_back({song.coverUrl, 600, 600});
    r.track.addedAt = song.addedAt;
    r.found = true;
    r.byIsrc = byIsrc;
    ++c.found;
    if (byIsrc) ++c.byIsrc;
}

void matchSpotify(const std::vector<svc::Song>& songs, spotify::Api* api, std::vector<Result>& out, Counters& c, Health& h, const CT& ct) {
    if (!api) return;
    parallel(songs.size(), 2, ct, [&](size_t i) {
        const auto& s = songs[i];
        if (!h.gaveUp) {
            try {
                if (!s.isrc.empty()) {
                    const auto b = api->searchMore(spotify::SearchKind::Tracks, "isrc:" + s.isrc, 0, 5, ct);
                    if (const int k = best(s, b.results.tracks, kAcceptIsrc); k >= 0) found(out[i], b.results.tracks[static_cast<size_t>(k)], true, c, s);
                    sleepFor(std::chrono::milliseconds(100), ct);
                }
                if (!out[i].found) {
                    const auto b = api->searchMore(spotify::SearchKind::Tracks, searchTitle(s.title) + " " + mainArtist(s), 0, 10, ct);
                    if (const int k = best(s, b.results.tracks, kAccept); k >= 0) found(out[i], b.results.tracks[static_cast<size_t>(k)], false, c, s);
                    sleepFor(std::chrono::milliseconds(100), ct);
                }
                h.ok();
            } catch (const OperationCanceled&) {
                throw;
            } catch (const std::exception& e) {
                if (ct.isCancellationRequested()) throw OperationCanceled();
                h.fail("spotify", s.title, e);
            }
        }
        ++c.done;
    });
}

void matchYtm(const std::vector<svc::Song>& songs, std::vector<Result>& out, Counters& c, Health& h, const CT& ct) {
    parallel(songs.size(), 4, ct, [&](size_t i) {
        const auto& s = songs[i];
        for (int attempt = 0; attempt < 2 && !h.gaveUp; ++attempt) {
            try {
                const auto r = ytm::searchKind(searchTitle(s.title) + " " + mainArtist(s), ytm::SearchKind::Songs, ct);
                if (const int k = best(s, r.tracks, kAccept); k >= 0) found(out[i], r.tracks[static_cast<size_t>(k)], false, c, s);
                h.ok();
                break;
            } catch (const OperationCanceled&) {
                throw;
            } catch (const ytm::ApiError& e) {
                if (e.status == 429 && attempt == 0) {   // slow down once
                    sleepFor(std::chrono::seconds(4), ct);
                    continue;
                }
                h.fail("ytm", s.title, e);
                break;
            } catch (const std::exception& e) {
                if (ct.isCancellationRequested()) throw OperationCanceled();
                h.fail("ytm", s.title, e);
                break;
            }
        }
        ++c.done;
    });
}

void matchMusicBrainz(const std::vector<svc::Song>& songs, std::vector<Result>& out, Counters& c, Health& h, const CT& ct) {
    // 1. ISRCs, 20 to a search.
    std::vector<size_t> withIsrc;
    for (size_t i = 0; i < songs.size(); ++i)
        if (!songs[i].isrc.empty()) withIsrc.push_back(i);
    constexpr size_t kBatch = 20;
    for (size_t b = 0; b < withIsrc.size() && !h.gaveUp; b += kBatch) {
        ct.throwIfCancellationRequested();
        std::vector<std::string> isrcs;
        for (size_t k = b; k < std::min(withIsrc.size(), b + kBatch); ++k) isrcs.push_back(songs[withIsrc[k]].isrc);
        try {
            const auto byIsrc = mb::recordingsByIsrc(isrcs, ct);
            for (size_t k = b; k < std::min(withIsrc.size(), b + kBatch); ++k) {
                const size_t i = withIsrc[k];
                const auto it = byIsrc.find(songs[i].isrc);
                if (it == byIsrc.end()) continue;
                if (const int x = best(songs[i], it->second, kAcceptIsrc); x >= 0) {
                    found(out[i], it->second[static_cast<size_t>(x)], true, c, songs[i]);
                    ++c.done;
                }
            }
            h.ok();
        } catch (const OperationCanceled&) {
            throw;
        } catch (const std::exception& e) {
            if (ct.isCancellationRequested()) throw OperationCanceled();
            h.fail("mb isrc", isrcs.front(), e);
        }
    }
    // 2. The rest by name (one request a second: at most kMaxNameSearches).
    int searches = 0;
    for (size_t i = 0; i < songs.size(); ++i) {
        if (out[i].found) continue;
        ct.throwIfCancellationRequested();
        const auto& s = songs[i];
        if (!h.gaveUp && searches < kMaxNameSearches) {
            ++searches;
            try {
                const auto tracks = mb::searchTracks(searchTitle(s.title) + " " + mainArtist(s), 10, ct);
                if (const int k = best(s, tracks, kAccept); k >= 0) found(out[i], tracks[static_cast<size_t>(k)], false, c, s);
                h.ok();
            } catch (const OperationCanceled&) {
                throw;
            } catch (const std::exception& e) {
                if (ct.isCancellationRequested()) throw OperationCanceled();
                h.fail("mb search", s.title, e);
            }
        }
        ++c.done;
    }
}

} // namespace

std::vector<Result> matchAll(const std::vector<svc::Song>& songs, Catalog catalog, spotify::Api* api, Counters& counters,
                             const CT& ct, bool* interrupted) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<Result> out(songs.size());
    for (size_t i = 0; i < songs.size(); ++i) out[i].track = nameOnly(songs[i]);
    Health health;
    switch (catalog) {
    case Catalog::Spotify: matchSpotify(songs, api, out, counters, health, ct); break;
    case Catalog::YouTubeMusic: matchYtm(songs, out, counters, health, ct); break;
    case Catalog::MusicBrainz: matchMusicBrainz(songs, out, counters, health, ct); break;
    }
    counters.done = static_cast<int>(songs.size());
    if (interrupted) *interrupted = health.gaveUp;
    ST_LOG_INFO(kTag, "{}: {} of {} found ({} by ISRC) in {} ms{}", toUtf8(catalogName(catalog)), counters.found.load(), songs.size(),
                counters.byIsrc.load(),
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
                health.gaveUp ? " (gave up after failures)" : "");
    return out;
}

std::string findAlbum(const svc::List& album, Catalog catalog, spotify::Api* api, const CT& ct) {
    const std::string artist = album.owner.substr(0, album.owner.find(", "));
    const std::string query = searchTitle(album.title) + " " + artist;
    auto pick = [&](const std::vector<catalog::Album>& albums, double minScore) -> std::string {
        std::string id;
        double top = minScore - 1;
        for (const auto& a : albums) {
            const double s = albumScore(album.title, artist, a);
            if (!a.id.empty() && s > top) {
                top = s;
                id = a.id;
            }
        }
        return id;
    };
    switch (catalog) {
    case Catalog::Spotify:
        if (!api) return {};
        if (!album.upc.empty())
            if (auto id = pick(api->searchMore(spotify::SearchKind::Albums, "upc:" + album.upc, 0, 5, ct).results.albums, kAcceptIsrc); !id.empty())
                return id;
        return pick(api->searchMore(spotify::SearchKind::Albums, query, 0, 10, ct).results.albums, kAccept);
    case Catalog::YouTubeMusic: return pick(ytm::searchKind(query, ytm::SearchKind::Albums, ct).albums, kAccept);
    case Catalog::MusicBrainz:
        if (!album.upc.empty())
            if (auto id = mb::releaseGroupByBarcode(album.upc, ct); !id.empty()) return id;
        return pick(mb::searchAlbums(query, 10, ct), kAccept);
    }
    return {};
}

} // namespace st::app::catmatch
