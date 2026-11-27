#pragma once
// The Spotify connect screen ("Nocturne" hero). Shown as a full-window overlay by the Shell when there is no
// Spotify session and the user hasn't chosen to browse the open catalog. "Spotify ile bağlan" opens the
// WebView2 login; "YouTube Music ile keşfet" / "MusicBrainz ile keşfet" set that open catalog
// (Settings::openCatalog) and dismiss the overlay.
#include "app/AppContext.h"
#include "gfx/Text.h"
#include "ui/Widget.h"

namespace st::ui {
class Button;
}

namespace st::app {

class ConnectScreen : public ui::Widget {
public:
    ConnectScreen();
    void layout() override;
    void paint(ui::Canvas& c) override;

private:
    ui::Button* connect_;
    ui::Button* ytm_;
    ui::Button* skip_;
    gfx::Text brand_, tag_, title1_, title2_, subtitle_, stepsHead_, without_;
    gfx::Text steps_[3];
    // Geometry from layout(), for paint(): it follows the window (a 960 DIP minimum, 150 % zoom, long languages). The
    // hero takes the largest display size whose lines fit its column; the brand lockup gives way on short windows (the
    // title bar names the app there).
    bool lockup_ = true;
    float heroSize_ = 64;
    gfx::Rect hero1_{}, hero2_{}, sub_{}, stepsHeadR_{}, stepR_[3]{}, withoutR_{};
    Lifetime life_;
};

} // namespace st::app
