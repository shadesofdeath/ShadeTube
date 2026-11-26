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
    float withoutY_ = 0;   // the "Spotify olmadan keşfet" caption over the open catalogs' buttons
    Lifetime life_;
};

} // namespace st::app
