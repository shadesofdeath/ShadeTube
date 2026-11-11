#pragma once
// Phone remote control (app/RemoteControl): what the phone is told, as JSON. The player state ("state" event), the
// queue window ("queue" event), the position tick ("pos" event) and the catalog items of search / album / playlist /
// artist answers. Pure: catalog models in, text out.
//
// Covers: an http(s) image URL is passed through (Spotify / Cover Art Archive images load straight from the phone);
// anything else goes through the CoverMapper, which the app points at its token-protected cover endpoint for the
// file:/// covers it extracted itself (local files, downloads), so no file system path ever reaches the phone.
//
// Standalone (catalog only): tests compile it directly.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::app::remote {

using CoverMapper = std::function<std::string(const std::string& url)>;

inline constexpr int kCoverPx = 640;   // now playing / collection headers
inline constexpr int kThumbPx = 120;   // list rows

// The URL the phone loads for the best image of at least `minWidth` px ("" = none).
std::string coverUrl(const std::vector<catalog::Image>& images, int minWidth, const CoverMapper& covers);

nlohmann::json trackJson(const catalog::Track& t, const CoverMapper& covers, int coverPx = kThumbPx);
nlohmann::json albumJson(const catalog::Album& a, const CoverMapper& covers, int coverPx = kThumbPx);
nlohmann::json artistJson(const catalog::Artist& a, const CoverMapper& covers, int coverPx = kThumbPx);
// `id` as the phone asks for it again ("liked" for Liked Songs), `count` = what the row shows ("" = nothing).
nlohmann::json playlistJson(const catalog::Playlist& p, const std::string& id, const std::string& count,
                            const CoverMapper& covers, int coverPx = kThumbPx);

struct NowPlaying {
    const catalog::Track* track = nullptr;   // nullptr = nothing queued
    std::string status = "idle";             // idle | resolving | buffering | playing | paused | error
    bool playing = false;                    // playing, buffering or resolving: the play button offers pause
    bool live = false;                       // an internet radio station (no seeking, no duration)
    bool podcast = false;
    std::string liveTitle;                   // a station's current song ("Artist - Title"), "" = none
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    float speed = 1;
    std::string shuffle = "off";             // off | on | smart
    std::string repeat = "off";              // off | all | one
    bool liked = false;
    bool canLike = false;
    bool hasNext = false;
    float volume = 0.7f;
    std::string accent;                      // "#RRGGBB": the app's accent color right now ("" = the theme's)
    std::string contextName;                 // what the queue plays ("Gece Sürüşü")
};
std::string stateJson(const NowPlaying& s, const CoverMapper& covers);

// The queue as the phone shows it: the playing slot and a window of what follows, each with its play-order index
// (jump / remove name the index and the track id, so a command for a queue that changed meanwhile is refused).
struct QueueView {
    int current = -1;                                           // play-order index of the playing item, -1 = none
    std::vector<std::pair<int, const catalog::Track*>> items;   // the current item first (when there is one)
    int more = 0;                                               // items after the window
    std::string moreText;                                       // "ve 12 şarkı daha" (translated), "" when more == 0
};
std::string queueJson(const QueueView& q, const CoverMapper& covers);

// The position tick: {"p": ms, "playing": bool, "line": "<the synced lyrics line>"}.
std::string positionJson(int64_t positionMs, bool playing, std::string_view lyricLine);

// Hex "#RRGGBB" from 0..1 components.
std::string hexColor(float r, float g, float b);

} // namespace st::app::remote
