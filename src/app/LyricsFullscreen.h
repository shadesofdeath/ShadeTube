#pragma once
// Full-screen karaoke lyrics (ctx().toggleLyricsFullscreen): a modal overlay over the whole main window on Now Playing's
// drifting backdrop in the cover's colours, large centered lines, the active one filled with the accent as it is sung (per word when
// the lyrics have word times, else across the line until the next one starts), a thin progress bar and the track.
// The top controls (lyrics source, "Yanlış sözler mi?", translation, timing offset −/+, share, close) fade out while
// the mouse rests. Esc / F
// close it, − / + shift the timing, a click on a line (or its translation) seeks there, the wheel scrolls for a
// moment. Everything it holds (backdrop, lyrics, text layouts) goes with it when it closes.
#include "app/Components.h"
#include "app/CoverBackdrop.h"
#include "app/LyricsService.h"
#include "lyrics/Lyrics.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Window.h"

#include <string>
#include <vector>

namespace st::app {

// It opens and closes with the overlay family's motion (fade in 240 ms, fade out 140 ms; none with Hareketi azalt or
// Windows' animation effects off).
class LyricsFullscreen : public ui::Widget, public ui::OverlayLayer {
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
    bool exiting() const override { return closing_ && exit_.running(); }

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
    double paintBackdrop(Canvas& c, const Rect& r);   // the delay until its next drift step (0 = still)
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
    ui::SmoothScroll scroll_;            // follows the active line; the wheel takes over for 3 s (manualUntil_)
    ui::Anim open_, controls_, exit_;
    bool closing_ = false;
    double manualUntil_ = 0;
    double lastMove_ = 0;
    Rect lyricsArea_{};

    ui::Widget* drag_;                   // the top strip moves the window (the title bar is covered)
    ui::Button *translate_, *earlier_, *offset_, *later_, *close_;
    ui::Button* share_;                  // "Sözleri paylaş" (app/LyricsShare)
    ui::Button* picker_;                 // "Yanlış sözler mi?" (app/LyricsPicker)
    lyrics::Choice pick_;                // the user's pick for this track
    Lifetime subs_;                      // the lyrics-choice subscription

    CoverBackdrop backdrop_;             // Now Playing's drifting cover colours (app/CoverBackdrop)
    gfx::Text title_, artist_, time_, label_;
};

} // namespace st::app
