#pragma once
// Spotify radio + endless playback + the kara liste's player wiring (app/Radio.cpp). The public entry points
// radioAvailable() / startRadio() / initPlaybackFeatures() are declared in AppContext.h.
//
// Radio: Api::radioPlaylist(seed) (the web player's "Go to song radio", spclient seed_to_playlist) -> that playlist's
// first 100 tracks -> kara liste filtered -> replaces the queue (context "playlist:<radio uri>", "<name> radyosu").
// Track, album and artist seeds have radios (an album gets its artist's); Spotify serves none for playlists, so a
// playlist is seeded with its first unblocked, playable tracks. Starting the radio of the song that is playing keeps
// it playing and replaces only what comes after it. A result is dropped when the user started another queue (or
// logged out) while it loaded.
// Endless playback (Settings::endlessPlayback): when a track starts with at most one playable item after it and
// repeat is off (Player::onQueueLow), the radio of the current Spotify track (else the last Spotify track of the
// queue) is appended — only tracks not in the queue yet and not blocked. One fetch at a time; each seed is tried
// once per queue (Player::queueGeneration) and at most kEndlessSeedsPerQueue seeds, so a radio that brings nothing
// new never loops. The result is re-checked against the setting, repeat and the login before it is appended.
#include <string>

namespace st::app {

// The radio seed for a catalog id: the id itself when it is a Spotify track / album / artist / playlist URI
// ("spotify:<kind>:<base62>"), a YouTube video ("yt:<id>": YouTube Music's song radio) or a YouTube Music album /
// artist / playlist ("ytm:*"), else "" — MusicBrainz ids, "local:" files, Liked Songs. Callers hide their radio entry
// for "". YouTube Music seeds get YouTube Music's automix queue (ytm::radio; context "ytmradio:<seed>"), also while
// logged in to Spotify; they need no account.
std::string radioSeed(const std::string& id);

// Endless playback: checks now whether the queue needs more (e.g. right after the setting was turned on). Only the
// player's own queue-low hook (fromPlayer) may resume a queue that has already ended.
void endlessCheck(bool fromPlayer = false);

} // namespace st::app
