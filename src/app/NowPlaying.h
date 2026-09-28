#pragma once
// Full-screen "Şimdi Çalıyor": blurred artwork backdrop, glowing hero art, synced lyrics, queue panel.
#include "app/Components.h"
#include "lyrics/Lyrics.h"
#include "ui/Anim.h"
#include "ui/Controls.h"

#include <optional>
#include <string>
#include <vector>

namespace st::app {

class QueuePanel;

class NowPlayingView : public ui::Widget {
public:
    NowPlayingView();
    void activate();     // start lyrics fetch for the current track
    void deactivate();   // release everything it holds (bitmaps, lyrics, text layouts)
    void onTrackChanged();

    void layout() override;
    void paint(Canvas& c) override;
    bool onWheel(float dy, float, const ui::MouseEvent& e) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override { hoverLine_ = -1; matchHover_ = false; }
    LPCWSTR cursor() const override { return (hoverLine_ >= 0 || matchHover_) ? IDC_HAND : IDC_ARROW; }

private:
    enum class LyricsState { None, Loading, Synced, Plain, Instrumental, Missing };
    void fetchLyrics();
    void paintBackdrop(Canvas& c, const Rect& r);
    void paintArtShadow(Canvas& c);
    void paintLyrics(Canvas& c, const Rect& r);
    float lineHeight(int i, float width);
    int lineAt(gfx::Point p) const;

    QueuePanel* queue_;
    Lifetime life_;
    std::string trackId_;
    LyricsState state_ = LyricsState::None;
    lyrics::Lyrics lyrics_;
    std::vector<gfx::Text> lines_;       // inactive style; a line keeps its layout only while it is on screen
    std::vector<float> lineH_;           // measured heights (-1 = not yet) for width lineHW_
    float lineHW_ = -1;
    gfx::Text activeText_;
    int active_ = -2;
    ui::Anim scroll_;
    double manualUntil_ = 0;
    float manualOffset_ = 0;
    std::vector<Rect> lineRects_;        // last painted (for hit testing)
    int hoverLine_ = -1;
    Rect artRect_{}, lyricsRect_{}, matchRect_{};
    bool matchHover_ = false;

    // Backdrop cache (quarter resolution; it is blurred anyway).
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> backdrop_;
    std::string backdropUrl_;
    uint64_t backdropGen_ = 0;
    uint64_t backdropTheme_ = 0;         // gfx::Theme::generation() it was built with
    float backdropW_ = 0, backdropH_ = 0;
    ui::Anim backdropFade_;

    // Hero art shadow, computed once on the CPU (see paintArtShadow).
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> shadow_;
    float shadowArt_ = 0;                // art size (DIP) it was built for
    uint64_t shadowGen_ = 0, shadowTheme_ = 0;
    double openedAt_ = 0;                // ui::frame::realNow() at activate()
    bool trimmed_ = true;                // the settle trim ran for this opening (see paint)
    gfx::Text title_, subtitle_, meta_;
};

} // namespace st::app
