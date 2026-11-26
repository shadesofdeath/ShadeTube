#pragma once
// Blend: one shared playlist made from the taste of two or more people, without a server and without accounts. Each
// member's app turns its own listening history (app/ListenStats) into a small taste profile — the 50 songs and 30
// artists it streamed most in the last six months, each with a relative weight, nothing else — shows it to the user,
// and exchanges it end-to-end encrypted over the listen-together transport (app/Blends: retained, sealed messages on a
// public MQTT broker). Every member then builds the same playlist from the same profiles (build() uses nothing but the
// profiles): the songs two or more of them love first, then one pick from each member in turn, every row marked with
// whose taste it is, plus a taste match percentage.
//
// The pure part lives here (tests/blend compiles it): the profile, its JSON, the mix, the match and the store's JSON.
//
// Determinism: songs and artists are matched by the key the *sender* computed ("k": 16 hex digits of FNV-1a-64 over
// ListenStats' folded "title + first artist" / artist name), never by keys computed again here, so two app versions
// that fold names differently still agree on the playlist. Ties are broken by key, members are ordered by their id.
//
// Standalone: core + catalog + app/ListenStats + nlohmann/json. Pure, any thread.
#include "app/ListenStats.h"
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st::app::blend {

inline constexpr int kProfileVersion = 1;
inline constexpr size_t kProfileTracks = 50;     // songs in a profile at most
inline constexpr size_t kProfileArtists = 30;    // artists in a profile at most
inline constexpr int64_t kWindowDays = 180;      // "the last six months"
inline constexpr size_t kMinWindowTracks = 15;   // fewer songs in the window: the whole history is used instead
inline constexpr size_t kMinProfileTracks = 5;   // fewer: not enough to blend
inline constexpr size_t kMaxMembers = 10;
inline constexpr size_t kMaxRows = 50;           // songs in the playlist at most

struct TasteTrack {
    std::string key;                          // 16 hex: the sender's song key (see above)
    std::string id;                           // catalog id: spotify:track:..., an MBID, yt:..., import:...
    std::string title;
    std::vector<catalog::ArtistRef> artists;  // at most 4
    std::string album, albumId;
    std::string cover;                        // http(s) image URL or ""
    int64_t durationMs = 0;
    int weight = 0;                           // 1..100, relative to the member's top song
    bool operator==(const TasteTrack& o) const;
};

struct TasteArtist {
    std::string key;                          // 16 hex: the sender's artist key
    std::string id, name;
    int weight = 0;                           // 1..100
    bool operator==(const TasteArtist&) const = default;
};

struct Profile {
    std::string member;                       // 16 hex: this member's random id in this blend (not a device id)
    std::string name;                         // the name others see (Settings::jamName or its default)
    int64_t at = 0;                           // unix seconds, rounded down to the hour
    int windowDays = static_cast<int>(kWindowDays);   // 0 = the whole history (too little in the window)
    std::vector<TasteTrack> tracks;           // heaviest first
    std::vector<TasteArtist> artists;         // heaviest first
    bool operator==(const Profile&) const = default;
};

std::string keyOf(const std::wstring& foldedKey);   // 16 hex digits of FNV-1a-64 over its UTF-8

// The taste of one history at `now` (unix seconds): streams only (ListenStats' rule), podcast episodes and stations
// left out, recent streams weigh more (half-life 60 days; a year over the whole history). `member` / `name` are left
// empty for the caller. Fewer than kMinProfileTracks songs: the profile is returned anyway; usable() says no.
Profile makeProfile(const std::vector<ListenStats::Track>& tracks, const std::vector<ListenStats::Play>& plays, int64_t now);
bool usable(const Profile& p);

nlohmann::json toJson(const Profile& p);
// Hostile input (it comes from another member): types checked, sizes capped, keys / member ids validated, covers
// kept only when http(s). nullopt when it isn't a profile.
std::optional<Profile> profileFromJson(const nlohmann::json& j);

struct Row {
    catalog::Track track;
    std::vector<int> from;                    // members (indexes into Mix::members) whose taste it is; 2+ = shared
};

struct Mix {
    std::vector<std::string> members;         // member ids, in the order the mix uses (sorted)
    std::vector<std::string> names;           // their names
    std::vector<Row> rows;
    int shared = 0;                           // rows liked by 2+ members (they come first)
    int match = 0;                            // taste match, 0..100 (-1 with fewer than two members)
};
// The same playlist (members, order, ids, titles, whose taste each row is, match).
bool sameMix(const Mix& a, const Mix& b);

// The playlist of these profiles (any order, duplicates by member dropped: the newest wins; at most kMaxMembers by
// member id). Deterministic: the same profiles give the same mix on every PC.
Mix build(std::vector<Profile> profiles);
// Taste match of two or more profiles: the mean over pairs of a weighted overlap of their artists (70 %) and songs
// (30 %), shown on a gentle curve (1 - (1 - overlap)^3) so that people who share a fifth of their taste read about
// half. -1 with fewer than two.
int tasteMatch(const std::vector<Profile>& profiles);

// ---- The store's record of one blend (app/Blends keeps them in blends.dat, sealed with DPAPI) ------------------------
struct Blend {
    std::string link;                         // the invite (shadetube://blend/...): blend id, key, broker
    std::string me;                           // my member id in it
    std::optional<Profile> mine;              // what this PC shares (sent); nullopt = joined, the user hasn't agreed yet
    std::vector<Profile> others;              // the newest profile of every other member
    std::vector<std::string> left;            // members who left (they are not blended any more)
    bool creator = false;
    int64_t createdAt = 0;                    // unix seconds
    int64_t syncedAt = 0;                     // the last exchange that reached a broker
    int64_t othersChangedAt = 0;              // the last time another member's profile changed
};
nlohmann::json toJson(const Blend& b);
std::optional<Blend> blendFromJson(const nlohmann::json& j);
// The profiles the playlist is built from: mine (when shared) + the others that haven't left.
std::vector<Profile> profilesOf(const Blend& b);

} // namespace st::app::blend
