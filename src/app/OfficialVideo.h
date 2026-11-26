#pragma once
// Video mode's official video (DEVELOPMENT.md 3.42): a song that streams from an Art Track or an audio upload (one still
// picture) gets the picture of the artist's music video of the same song, silent, against ShadeTube's own audio.
//
//   decide(): the cached decision (app/OfficialVideoCache), else on a worker: candidates (youtube/OfficialVideo:
//   YouTube Music's OMV-labelled videos, then YouTube's search), the song's own soundtrack and each candidate's (the
//   smallest Opus stream: video/Soundtrack), aligned (video/AudioAlign). The first candidate
//   whose soundtrack is the song's recording wins; none: the cover, as before. Low priority; ~1.3 MB per soundtrack.
//   showPicker() / reject() / showNone(): "Yanlış video mu?" from video mode's menu (a pick is aligned before it is
//   used: a video that isn't the song's recording is never shown).
//
// UI thread (the work runs on the pool).
#include "app/OfficialVideoCache.h"
#include "catalog/Models.h"
#include "core/Async.h"

#include <functional>
#include <string>

namespace st::app::officialvideo {

struct Song {
    catalog::Track track;
    std::string audioVideoId;   // the upload the song streams from (Player::currentMatch)
    std::string region;         // the search's country (Settings::region; filled by decide() / the picker)
};

// The decision for the song (cached, else searched and aligned). `done` runs on the UI thread unless `guard` expired;
// an error means the lookup failed for a reason that may pass (network): nothing was stored.
void decide(const Song& song, Lifetime::Ref guard, std::function<void(Result<OfficialDecision>)> done);

// "Yanlış video mu?": the candidates with what is known about them; a pick is aligned first. `changed` runs when the
// song's decision changed (video mode loads it again).
void showPicker(const Song& song, std::function<void()> changed);
// "Bu videoyu gösterme": the current official video is turned down for this song; the next candidate is looked for.
void reject(const Song& song, std::function<void()> changed);

OfficialVideoCache& cache();

// Exit (video mode's persist hook): cancels the lookups on the pool and waits for them (at most 10 s), since App
// destroys the MatchService they use before it joins the pool.
void shutdown();

} // namespace st::app::officialvideo
