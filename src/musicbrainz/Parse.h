#pragma once
// Internal: pure JSON -> catalog model mapping and query building (no I/O). Exposed for offline tests.
// Every accessor tolerates missing keys / wrong types / nulls.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace st::mb::detail {

using json = nlohmann::json;
using namespace st::catalog;

// ---- safe accessors ------------------------------------------------------------------------------------------
const json& get(const json& j, std::string_view key);            // null json if missing / not an object
std::string str(const json& j, std::string_view key);            // "" unless string
int64_t num(const json& j, std::string_view key, int64_t def = 0);
const json& arr(const json& j, std::string_view key);            // empty array unless array

// ---- Lucene queries ------------------------------------------------------------------------------------------
bool hasFieldPrefix(std::string_view query);                     // "tag:rock", "artist:\"x\"" ... -> pass through
std::string luceneEscape(std::string_view text);
std::string artistQuery(const std::string& q);
std::string releaseGroupQuery(const std::string& q);
std::string recordingQuery(const std::string& q);

// ---- MusicBrainz ---------------------------------------------------------------------------------------------
std::vector<ArtistRef> parseArtistCredit(const json& credit);
std::vector<std::string> parseTags(const json& entity, size_t max = 8);   // sorted by vote count desc
Artist parseArtist(const json& a);                                        // search hit or lookup
std::vector<Artist> parseArtistSearch(const json& root, int limit);
std::string wikidataId(const json& artistLookup);                        // "Q485771" from url-rels, or ""
std::string commonsFileFromRels(const json& artistLookup);              // "image" rel to Commons, or ""
std::string wikidataImageFile(const json& entityData);                   // claim P18, or ""
std::vector<Image> commonsImages(const std::string& fileName);           // Special:FilePath 250/500/1000

Album parseReleaseGroup(const json& rg);                                 // search hit / browse / lookup
std::vector<Album> parseReleaseGroupSearch(const json& root, bool freeText, int limit);
std::vector<Album> parseReleaseGroupBrowse(const json& root);
bool isOtherRelease(const Album& a);          // secondary Compilation/Live/Remix/DJ-mix/Demo
void sortNewestFirst(std::vector<Album>& albums);

struct RecordingHit {
    Track track;
    int releaseCount = 0;
    bool official = false;
    double score = 0;          // MusicBrainz search score
};
std::vector<RecordingHit> parseRecordingHits(const json& root);
std::vector<Track> rankRecordingSearch(std::vector<RecordingHit> hits, const std::string& query, bool freeText,
                                       int limit);
// Top tracks without ListenBrainz' auth-only endpoint: artist recordings + bulk popularity (recording_mbid ->
// total_listen_count). Groups versions of the same title, ranks by listens then by release count.
std::vector<Track> rankTopTracks(std::vector<RecordingHit> hits, const std::unordered_map<std::string, int64_t>& listens,
                                 int limit);
std::unordered_map<std::string, int64_t> parsePopularity(const json& root);

// Representative release of a release group lookup (inc=releases+media). "" if none.
std::string pickRepresentativeRelease(const json& rgLookup);
void applyRelease(Album& album, const json& release);   // tracks, label, totalTracks, releaseId

// ---- ListenBrainz --------------------------------------------------------------------------------------------
std::vector<Image> caaReleaseImages(const std::string& releaseMbid, int64_t caaId);
std::vector<Artist> parseLbArtists(const json& root, int limit);          // stats/sitewide/artists
std::vector<Album> parseLbReleaseGroups(const json& root, int limit);     // stats/sitewide/release-groups
std::vector<Track> parseLbRecordings(const json& root, int limit);        // sitewide recordings / top-recordings
std::vector<Album> parseFreshReleases(const json& root, int limit);
std::vector<Artist> parseSimilarArtists(const json& root, const std::string& selfId, int limit);

} // namespace st::mb::detail
