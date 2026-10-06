#pragma once
// Full-screen karaoke lyrics (ctx().toggleLyricsFullscreen): a modal overlay over the whole main window with the
// artwork's blurred backdrop, large centered lines, the active one filled with the accent as it is sung (per word when
// the lyrics have word times, else across the line until the next one starts), a thin progress bar and the track.
// The top controls (lyrics source, translation, timing offset −/+, close) fade out while the mouse rests. Esc / F
// close it, − / + shift the timing, a click on a line (or its translation) seeks there, the wheel scrolls for a
// moment. Everything it holds (backdrop, lyrics, text layouts) goes with it when it closes.
#include "app/Components.h"
#include "app/LyricsService.h"
#include "lyrics/Lyrics.h"
#include "ui/Anim.h"
#include "ui/Controls.h"

#include <string>
#include <vector>

namespace st::app {

class LyricsFullscreen : public ui::Widget {
public:
    static void toggle();          // opens over the main window, or closes it
    static bool isOpen();
    static void trackChanged();    // a new track started: reload (trackChangedHooks)

    LyricsFullscreen();
    ~LyricsFullscreen() override;
    void close();

    void layout() override;
    void paint(Canvas& c) override;
    bool onKeyDown(const ui::KeyEvent& e) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    bool onWheel(float dy, float, const ui::MouseEvent& e) override;
    LPCWSTR cursor() const override { return hoverLine_ >= 0 ? IDC_HAND : IDC_ARROW; }

private:
    enum class State { None, Loading, Synced, Plain, Instrumental, Missing, Live };
    void load();
    void syncControls();
    void rebuildTranslation();           // translated lines under the lyrics, for translation_'s state
    float translationHeight(int i, float width);   // with its gap above; 0 = nothing under line i
    void shiftOffset(int ms);
    void showControls();
    float lineHeight(int i, float width);
    int lineAt(gfx::Point p) const;
    void paintBackdrop(Canvas& c, const Rect& r);
    void paintLyrics(Canvas& c, const Rect& area);
    void paintSweep(Canvas& c, int i, const Rect& lr, int posMs);
    void paintFooter(Canvas& c, const Rect& r);
    void paintTop(Canvas& c, const Rect& r);

    Lifetime life_;
    std::string trackId_;
    State state_ = State::None;
    lyrics::Lyrics lyrics_;
    gfx::TextStyle style_{};
    std::vector<gfx::Text> lines_;       // a line keeps its layout only while it is on screen
    std::vector<float> lineH_;           // measured heights (-1 = not yet) for width lineHW_
    float lineHW_ = -1;
    LyricsTranslation translation_;      // "Çeviri"
    gfx::TextStyle transStyle_{};
    std::vector<gfx::Text> trans_;       // each line's translation while shown (empty text = none), like lines_
    std::vector<float> transH_;          // measured heights (-1 = not yet) for width transHW_
    float transHW_ = -1;
    std::vector<Rect> lineRects_;        // last painted (hit testing), translation included
    int active_ = -1;
    int hoverLine_ = -1;
    ui::Anim scroll_, open_, controls_;
    double manualUntil_ = 0;
    float manualOffset_ = 0;
    double lastMove_ = 0;
    Rect lyricsArea_{};

    ui::Widget* drag_;                   // the top strip moves the window (the title bar is covered)
    ui::Button *translate_, *earlier_, *offset_, *later_, *close_;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> backdrop_;
    std::string backdropUrl_;
    uint64_t backdropGen_ = 0, backdropTheme_ = 0;
    float backdropW_ = 0, backdropH_ = 0;
    ui::Anim backdropFade_;
    gfx::Text title_, artist_, time_, label_;
    gfx::DeviceHook deviceHook_{[this] { backdrop_.Reset(); }};   // idle release: must not keep the device alive
};

} // namespace st::app
