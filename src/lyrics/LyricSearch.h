#pragma once
// "Sözden şarkı bul": which song a remembered line of lyrics is from. The source is Genius' lyrics search (the web
// site's own JSON endpoint, genius.com/api/search/lyric: no key) - it matches fragments in any language, tolerates
// missing accents and typos, and names the matching passage, so a result can show the line it was found by. Measured
// on 2026-11-27 with 13 fragments in English, Turkish, Spanish and German: the right song first for all 13 (YouTube
// Music's search, the fallback in app/LyricSearch, had it among its first two rows for 12, sometimes only as a video,
// and names no passage; LRCLIB's search does not look into lyrics at all).
//
// The app resolves each match to a track of the active catalog (app/LyricSearch). Blocking: call from a worker
// thread. Standalone (core only): tests compile it directly (lyrictools_test, recorded answers).
#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::lyrics {

inline constexpr const char* kSourceGenius = "genius";

struct LyricMatch {
    std::string title;        // the song as the source names it (UTF-8)
    std::string artist;       // its primary artist(s) as shown ("The Chainsmokers & Coldplay")
    std::string mainArtist;   // the first primary artist (what a catalog search should look for)
    std::string imageUrl;     // a cover-sized picture (300 px), "" = none
    // The lines the query was found in (at most two, joined with " / "), UTF-8; "" = the source names none.
    std::string snippet;
    // The words of the snippet that matched, as [start, end) offsets in UTF-16 code units of the snippet (what
    // DirectWrite ranges count), sorted, not overlapping.
    std::vector<std::pair<uint32_t, uint32_t>> highlights;
    int exactWords = 0;       // query words found as typed
    int typos = 0;            // query words found only with a typo / without accents
    int64_t popularity = 0;   // the source's page views (orders equally good matches)
    std::string source;       // kSourceGenius
};

// Genius' /api/search/lyric answer -> its matches, the best first: translations and instrumental entries dropped,
// matches clearly worse than the best one (fewer exact words, more typos) dropped, equally good ones by popularity,
// one entry per song (title without its parenthesis + artist, case and accents ignored).
std::vector<LyricMatch> parseGeniusLyricSearch(const std::string& body);

// The snippet's lines for a context `value` with highlight `ranges` (UTF-16 offsets into it): the one line, or two
// neighbouring lines, holding most of the highlighted characters. Exposed for tests.
std::pair<std::wstring, std::vector<std::pair<uint32_t, uint32_t>>>
bestSnippet(const std::wstring& value, const std::vector<std::pair<uint32_t, uint32_t>>& ranges);

using SearchTransport = std::function<YoutubeExplode::Http::HttpResponse(const std::string& url,
                                                                          const YoutubeExplode::CancellationToken&)>;
// GET genius.com/api/search/lyric?q=<text>&per_page=10. nullopt = the request failed (offline, HTTP error, an answer
// that isn't the search's JSON: a bot check); an empty list = nothing found. `transport` unset = the shared client.
std::optional<std::vector<LyricMatch>> searchGenius(const std::string& text, const YoutubeExplode::CancellationToken& ct = {},
                                                    const SearchTransport& transport = {});

// Folds a title / name for comparing songs across sources: lower case, accents and Turkish dotted / dotless i folded,
// punctuation dropped, "&" = "and", whitespace collapsed; `dropBrackets` also leaves out "(...)" / "[...]" parts and a
// " - ..." tail (versions, remasters, features). UTF-8 in and out.
std::string foldTitle(const std::string& s, bool dropBrackets = true);

// A catalog track (its title and artist names) is the song a source named: the titles folded alike (one may run on by
// whole words: "Despacito" / "Despacito Remix") and one of the track's artists the source's main artist (or a whole-word
// start of it either way; a leading "The" ignored). An empty `artist` matches any.
bool sameSong(const std::string& trackTitle, const std::vector<std::string>& trackArtists, const std::string& title,
              const std::string& artist);

// A search query that reads like a line of lyrics rather than a name: at least three words, or 18 letters.
bool looksLikeLyrics(std::wstring_view query);

} // namespace st::lyrics
