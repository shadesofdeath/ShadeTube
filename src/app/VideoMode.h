#pragma once
// Video mode ("Video modu"): the music video of the playing song, shown silent next to the app's own audio, which stays
// the master (equalizer, normalisation, crossfade, speed, listen together, SponsorBlock: all unchanged). The picture
// follows the engine's heard position (AudioEngine::clock) through st_video (VideoPlayer / SyncScheduler).
//
// - The stream: the same YouTube video the song resolved to (Player::currentMatch), its picture-only stream up to
//   Settings::videoQuality (MatchService::videoStream, youtube/VideoStreams: H.264 first). A still picture (an Art
//   Track, an audio upload with one image), a video without a usable stream and items that don't stream from YouTube
//   show the cover with "Bu şarkı için video yok"; podcasts and radio have no button at all.
// - Where: VideoView widgets. Now Playing swaps its big cover for one (the Now Playing hook is a VideoView child laid
//   over the art rect), and the video window (app/VideoWindow: resizable, always-on-top option, full screen) has one.
// - Resources: the player (decoder thread, its GPU surfaces, the download) exists only while video mode is on, the
//   item has a video and a VideoView is on screen (its window shown and not minimized, every parent visible). It is
//   made when the audio already plays (the video download never competes with the song's start) and dropped within
//   ~2 s of the last view going away; the idle graphics release and device loss drop it too (it is made again at the
//   audio's position when a view shows up). The download stays a few seconds ahead of the picture.
// - Controls: the player bar's video button, the `video-mode` / `video-window` actions (palette, Ctrl+Shift+V),
//   Ayarlar › Oynatma (quality cap, remember video mode).
#include "gfx/Canvas.h"
#include "gfx/Types.h"

#include <string>

namespace st::ui {
class Window;
}

namespace st::app {

class VideoView;

namespace videomode {

bool on();
void setOn(bool on, bool announce = false);   // announce: a toast says so (keys, palette)
// The button / shortcut: turned on while no view is on screen, it opens Now Playing (where the cover becomes the video).
void toggle(bool announce = false);
// The player bar's button: shown for songs (not podcasts / radio); enabled unless the song plays from a file or a
// stand-in source (no YouTube video to follow).
bool offered();
bool enabled();

// The video window (app/VideoWindow). Opening it turns video mode on.
void openWindow();
void closeWindow();
void toggleWindow();
bool windowOpen();

// Views register themselves; paint() draws the video (or the cover with a note) into `r` (the view's local space).
void attach(VideoView* view);
void detach(VideoView* view);
void paint(gfx::Canvas& c, const gfx::Rect& r, const VideoView& view);
// The view shows video mode's content (on and the item offered) instead of what it replaces (Now Playing's cover).
bool showing();

// Re-evaluates what should exist (player, stream) now: player / track changes, views shown or hidden, settings.
void sync();

} // namespace videomode
} // namespace st::app
