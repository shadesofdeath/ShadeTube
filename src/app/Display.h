#pragma once
// "Arayüz ölçeği" (UI zoom) and "Liste yoğunluğu" (track list density), both in Ayarlar › GÖRÜNÜM. UI thread only.
//
// Zoom: Settings::uiZoom, a percentage from kZoomSteps, multiplies the main window's DPI scale (ui::Window::setZoom):
// the window keeps its pixel size and the content reflows, independent of Windows' own scaling (a monitor change keeps
// it). It changes with the zoom-in / zoom-out / zoom-reset actions (Ctrl+Plus, Ctrl+Minus, Ctrl+0 by default; the
// numeric keypad's + / - / 0 as well, see shortcuts::fallback), Ctrl + mouse wheel and the settings row. Keyboard and
// wheel changes show a short badge with the new value. The mini player stays at 100 % (its fixed layout is sized for
// the DPI alone).
//
// Density: Settings::compactLists. TrackTable (app/Components) reads it at every layout / paint: Rahat = 56 DIP rows
// with 40 DIP art, Sıkı = 40 DIP rows with 28 DIP art. A change re-lays out the open page (ui::Window::relayoutAll).
//
// The step arithmetic is constexpr (tests/shortcuts checks it). The settings rows: buildDisplayRows()
// (app/SettingsWidgets.h).

namespace st::app::display {

inline constexpr int kZoomSteps[] = {80, 90, 100, 110, 125, 150};
inline constexpr int kZoomStepCount = static_cast<int>(sizeof kZoomSteps / sizeof kZoomSteps[0]);

// Index of the step nearest to `percent` (the lower one on a tie): a hand-edited settings.json.
constexpr int zoomIndex(int percent) {
    int best = 0;
    for (int i = 1; i < kZoomStepCount; ++i) {
        const int d = kZoomSteps[i] - percent, bd = kZoomSteps[best] - percent;
        if ((d < 0 ? -d : d) < (bd < 0 ? -bd : bd)) best = i;
    }
    return best;
}
constexpr int snapZoom(int percent) { return kZoomSteps[zoomIndex(percent)]; }
// One step up (dir > 0) or down (dir < 0) from `percent`; stays at the ends.
constexpr int stepZoom(int percent, int dir) {
    const int i = zoomIndex(percent) + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
    return kZoomSteps[i < 0 ? 0 : i >= kZoomStepCount ? kZoomStepCount - 1 : i];
}

int zoom();                                // Settings::uiZoom, snapped
// Saves the zoom and applies it to the main window. `feedback`: show the badge (keyboard, wheel).
void setZoom(int percent, bool feedback);
bool zoomBy(int dir);                      // one step (the shortcut actions); shows the badge
// The main window's ui::Window::onCtrlWheel: wheel notches add up to a step (a touchpad pinch sends small ones).
bool onCtrlWheel(float delta);

bool compactLists();                       // Settings::compactLists
void setCompactLists(bool on);             // saves and re-lays out the main window

// Dev check without a keyboard (docs/DEVELOPMENT.md 5.4): SHADETUBE_KEYS_DEMO="<ms>:<combo>[,<combo>…]" presses the key
// combos (text form, "Ctrl+NumAdd", "Ctrl+Shift+4") through the main window's shortcut dispatch, the first after <ms>
// and the rest 300 ms apart; "action:<id>" runs that action. App calls it once the main window exists.
void scheduleKeysDemo();

} // namespace st::app::display
