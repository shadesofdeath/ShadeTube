#pragma once
// Song preview ("Önizleme"): press and hold a track row or a card's cover to hear about 20 s of the song without
// touching what plays or the queue; also the track menu's "Önizle" and the "Önizle" key for the focused row. The state
// machine is app/PreviewSession (standalone, tested); this is the app side: the hold (ui::Window::onHold), the sources
// below, the collection lookups, the overlay, the settings rows and the player (Player::startPreview). See
// docs/DEVELOPMENT.md ("Song preview").
#include "app/PreviewSession.h"
#include "app/Router.h"
#include "gfx/Types.h"
#include "ui/Popups.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace st::ui {
class Column;
}

namespace st::app {

// What a press previews and where it was (window DIPs: the row / cover shows the pressed look, the overlay sits by the
// pointer).
struct PreviewHit {
    preview::Target target;
    gfx::Rect rect;
};

// A widget that can be previewed: track tables (a row), cards (the cover), the Library's cells, the queue's rows. The
// window asks the innermost one above the pressed widget when a press has rested long enough; the "Önizle" key asks
// the focused one with keyboard = true (its keyboard cursor / itself; windowPos is then meaningless).
class PreviewSource {
public:
    virtual ~PreviewSource() = default;
    virtual std::optional<PreviewHit> previewAt(gfx::Point windowPos, bool keyboard) = 0;
};

namespace preview {

Mode currentMode();   // Settings::previewMode
// A card's target from the route it opens: an album, a playlist, an artist or Liked Songs (nullopt for anything else).
std::optional<Target> collectionTarget(const Route& open, const std::wstring& title, const std::vector<catalog::Image>& images);
// The track menu's "Önizle" (or "Önizlemeyi durdur" while that song previews) for one song; nullopt when previews are
// off, for several songs, a station / episode or the song that plays.
std::optional<ui::MenuItem> menuItem(const std::vector<catalog::Track>& tracks, gfx::Point windowPos);
// App's Esc: stops a running preview first (true = taken).
bool handleEscape();

} // namespace preview

} // namespace st::app
