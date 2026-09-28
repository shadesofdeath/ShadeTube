#pragma once
// The Spotify connect screen ("Nocturne" hero). Shown as a full-window overlay by the Shell when there is no
// Spotify session and the user hasn't chosen to browse the open catalog. "Spotify ile bağlan" opens the
// WebView2 login; "Spotify olmadan keşfet" dismisses the overlay and uses MusicBrainz.
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
    ui::Button* skip_;
    gfx::Text brand_, tag_, title1_, title2_, subtitle_, stepsHead_;
    gfx::Text steps_[3];
    Lifetime life_;
};

} // namespace st::app
