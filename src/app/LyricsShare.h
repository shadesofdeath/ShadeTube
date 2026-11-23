#pragma once
// "Sözleri paylaş": a lyrics card for the playing song. The share button of Now Playing's and the full-screen lyrics'
// header calls openLyricsShare(); everything else lives here. A dialog lets the user select 1..6 consecutive lines
// (click a line, click its neighbours to extend, Shift+click for a range; the keyboard: Up / Down, Space / Enter,
// Shift+Up / Down) next to a live preview of the card: the lines in large type (the translation under each when the
// lyrics view shows one, switchable), the cover, title and artist and the ShadeTube mark, on a ground from the cover
// (its colors as a gradient, the cover blurred, or plain dark / light paper), square / story (9:16) / wide (16:9),
// left-aligned or centered. "Görseli kopyala" puts it on the clipboard and "PNG olarak kaydet…" saves it, both at
// 1080 px on the short edge (app/ShareCard), whatever the window size or interface scale. Synced and unsynced
// lyrics alike. The choices are kept for the session.
#include "app/LyricsService.h"
#include "app/ShareCard.h"
#include "lyrics/Lyrics.h"

#include <string>
#include <vector>

namespace st::app {

// Opens the dialog for the current track's lyrics `l` (as the view has them). `activeLine`: the line to start from
// (the sung one; -1 = the first lines). `translation`: the view's, its lines are used while it shows them.
void openLyricsShare(const lyrics::Lyrics& l, int activeLine, const LyricsTranslation& translation);

namespace share {

struct LyricsCard {
    enum class Style { Gradient, Blurred, Dark, Light };
    enum class Ratio { Square, Story, Wide };
    Style style = Style::Gradient;
    Ratio ratio = Ratio::Square;
    bool centered = false;
    std::vector<std::wstring> lines;         // the selected lines
    std::vector<std::wstring> translations;  // parallel to lines ("" = none); empty = no translations
    std::wstring title, artist;
    std::vector<catalog::Image> images;      // the cover
};
// Card size in units (360 on the short edge).
gfx::Size cardUnits(LyricsCard::Ratio ratio);
void paintLyricsCard(Canvas& c, const Rect& r, CardContext& ctx, const LyricsCard& card);

} // namespace share

// Dev (docs/DEVELOPMENT.md 5.4): SHADETUBE_LYRICS_SHARE=<from>-<to>[,gradient|blur|dark|light][,square|story|wide]
// [,center][,trdemo][,copy][,png=<file>] opens the dialog for the first track that plays (its lyrics fetched here), with
// lines from..to selected (0-based) and those options; trdemo = a stand-in translation under every line, copy = "Görseli
// kopyala" right away, png=<file> also writes the export there. Called once from initShareFeatures().
void startLyricsShareDemo();

} // namespace st::app
