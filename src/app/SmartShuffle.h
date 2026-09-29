#pragma once
// Smart shuffle and "Geliştir" (Enhance), free: recommended songs that fit a collection, mixed into it
// (app/SmartShuffle.cpp; where they go is app/SmartMix).
//
// Smart shuffle (Settings::shuffle + Settings::smartShuffle; the shuffle button cycles off -> shuffle -> smart shuffle):
// while it is on, the upcoming part of the queue gets a recommendation after every 3-4 songs, topped up as the queue
// plays (a player hook plans the next smartmix::kWindow slots). Leaving smart shuffle drops the recommendations that
// haven't played yet (Player::dropRecommendations).
// "Geliştir" (Settings::enhancedCollections, per playlist / Liked Songs): the collection page shows recommendations
// interleaved in its track list, marked; "+" adds one to the collection for real, "−" hides it for this session.
// Playing the list plays them in order.
//
// Recommendations for a collection (a sample of its songs as seeds) are fetched on a worker and cached per collection
// for 30 minutes: Spotify's song radios of up to 3 of its Spotify songs (logged in), topped up with the popular
// recordings of ListenBrainz's similar artists for its most frequent MusicBrainz artists, then with local-library songs
// by its artists. A Spotify 429 pauses fetching for 10 minutes. Recommended tracks are ordinary catalog::Tracks with
// `recommended` set (the queue, its session and the badges keep it); once played they count like any play.
#include "catalog/Models.h"
#include "core/Async.h"
#include "gfx/Canvas.h"

#include <functional>
#include <string>
#include <vector>

namespace st::app::smartshuffle {

enum class Mode { Off, Shuffle, Smart };
Mode mode();
void setMode(Mode m);
void cycleMode();                    // the shuffle button: off -> shuffle -> smart shuffle -> off
std::wstring modeLabel(Mode m);      // "Karıştır" / "Akıllı karıştırma" (tooltips)

// "Geliştir". `key` = "liked" or the playlist id.
bool enhanced(const std::string& key);
void setEnhanced(const std::string& key, bool on);

// Recommendations for the collection `key` whose songs are `collection`: not in it, not hidden for `key`, unblocked
// and playable, in a stable order for the key. `done` runs on the UI thread (at once when cached) unless `guard`
// expired; an empty list = nothing found (or the fetch failed).
void recommend(const std::string& key, const std::vector<catalog::Track>& collection, Lifetime::Ref guard,
               std::function<void(std::vector<catalog::Track>)> done);
void hide(const std::string& key, const catalog::Track& t);   // "−": not recommended again this session
bool isHidden(const std::string& key, const catalog::Track& t);

// The collection the queue plays ("liked" or a playlist id), "" for anything else (an album, search results...).
std::string queueCollectionKey();
// Menus: whether `t` can be added to the collection `key` ("" = the queue's collection), and adding it (its queue items
// lose the badge).
bool canAdd(const std::string& key, const catalog::Track& t);
void add(const std::string& key, const catalog::Track& t);
// "Önerileri gösterme": Enhance off for `key` ("" = the queue's collection), and smart shuffle back to plain shuffle
// (or the queue's recommendations dropped) when the queue plays that collection.
void stop(const std::string& key);

// Enhance on / off, hides and adds (the collection pages follow them). The subscription dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

// The "ÖNERİLEN" chip (sparkle + label, accent) with its left edge at x, centered on cy; returns its width.
float drawBadge(gfx::Canvas& c, float x, float cy);
float badgeWidth();

} // namespace st::app::smartshuffle
