#pragma once
// The full-screen visualizer ("Görselleştirici", docs/DEVELOPMENT.md): a modal overlay over the whole main window on
// Now Playing's drifting cover backdrop (app/CoverBackdrop). What plays comes from the engine's waveform tap
// (AudioEngine::waveform) through audio/VisualAnalysis (bands, peaks, bass / mid / treble, beats, the oscilloscope's
// view), drawn in one of four modes in the theme's colours and the cover's:
//   Spektrum  thin bars on a baseline with held, falling peaks and a faint reflection
//   Halka     the round cover in the middle, the spectrum mirrored around it, a ring that leaves it on every beat
//   Dalga     the waveform as a tapered line across the view with a short trail (the seek wave's language)
//   Aurora    slow curtains of light in the cover's colours, lit by bass / mids / treble, with a few drifting motes
// The song (title, artist, a thin progress line and the time) sits in the bottom-left corner, the sung lyric line
// above it (optional, synced lyrics only). The top strip (mode chips, lyric line, full screen, close) fades after
// 2.5 s without the mouse and the pointer hides with it. Keys: 1-4 / M the modes, L the lyric line, F (or a double
// click) full screen, Esc closes. The mode and the lyric line are remembered (Settings::visualizerMode / Lyrics).
//
// Cost: it paints every display refresh while sound plays, up to ~125 a second (every second refresh at 240 Hz:
// ui::frame::requestNextEvery), with nothing else painted meanwhile (coversWindow()), and asks for no frames at all
// once a pause has settled; hidden or minimized windows never render. Reduce motion (or
// Windows' animation effects off): slow smoothing at 10 frames a second, no beat pulses, still curtains and motes.
// Everything it holds (the analysis, the curtain bitmap, lyrics, text layouts) goes with it when it closes.
#include "app/Components.h"
#include "app/CoverBackdrop.h"
#include "audio/VisualAnalysis.h"
#include "lyrics/Lyrics.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Window.h"

#include <array>
#include <string>
#include <vector>

namespace st::app {

class Visualizer : public ui::Widget, public ui::OverlayLayer {
public:
    enum class Mode { Bars, Ring, Wave, Aurora };
    static constexpr int kModes = 4;

    static void toggle();          // opens over the main window, or closes it
    static bool isOpen();
    static void trackChanged();    // a new track started (trackChangedHooks)
    static Mode savedMode();       // Settings::visualizerMode (SHADETUBE_VIZ_MODE overrides it for a run)
    static const char* modeId(Mode m);

    Visualizer();
    ~Visualizer() override;
    void close();

    void layout() override;
    void paint(Canvas& c) override;
    bool onKeyDown(const ui::KeyEvent& e) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    LPCWSTR cursor() const override;
    bool exiting() const override { return closing_ && exit_.running(); }
    bool coversWindow() const override;

private:
    void setMode(Mode m);
    void setLyricsShown(bool on);
    void setFullscreen(bool on);
    void syncTrack();
    void loadLyrics();
    void syncControls();
    void showControls();
    void analyse(bool& live);

    void paintMode(Canvas& c, Mode m, const Rect& area, float alpha);
    void paintBars(Canvas& c, const Rect& area, float alpha);
    void paintRing(Canvas& c, const Rect& area, float alpha);
    void paintWave(Canvas& c, const Rect& area, float alpha);
    void paintAurora(Canvas& c, const Rect& area, float alpha);
    void renderCurtains(Canvas& c, int w, int h);
    void paintCover(Canvas& c, const Rect& r, float alpha);
    void paintLyric(Canvas& c, const Rect& r);
    void paintCorner(Canvas& c, const Rect& r);
    void paintTop(Canvas& c, const Rect& r);
    void logStats(double paintMs);

    Lifetime life_;
    std::string trackId_;
    Mode mode_ = Mode::Ring, from_ = Mode::Ring;
    ui::Anim modeMix_{1.f};          // from_ -> mode_
    ui::Anim open_, exit_, controls_;
    bool closing_ = false;
    double lastMove_ = 0;
    double lastFrame_ = 0;           // ui::frame::now() of the previous paint

    // Analysis.
    audio::viz::Analyzer analyzer_;
    std::vector<float> samples_ = std::vector<float>(audio::viz::kHistory);
    bool motion_ = true;             // ui::motion::micro() at the last paint
    int lastBeats_ = 0;
    double drift_ = 0;               // seconds of motion so far (advances only while sound plays)

    // Halka: rings leaving the cover on beats; the cover itself.
    struct Pulse {
        double born;
        float strength;
    };
    std::vector<Pulse> pulses_;
    std::string coverUrl_;
    ui::Anim coverIn_{1.f};
    // Dalga: the last few waveforms (a trail), one kept every ~45 ms.
    std::vector<std::vector<float>> trail_;
    double trailAt_ = 0;
    std::vector<gfx::Point> wavePts_;
    // Aurora: the curtains (a small bitmap, recomputed ~30 times a second while sound plays) and the motes.
    std::vector<uint32_t> curtainTexels_;
    int curtainW_ = 0, curtainH_ = 0;
    double curtainAt_ = -1e9;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> curtains_;
    uint64_t curtainGen_ = 0;
    struct Mote {
        float x, y, speed, size, phase;
    };
    std::vector<Mote> motes_;

    // Lyrics (read-only: the provider chain of app/LyricsService), the sung line cross-fading.
    lyrics::Lyrics lyrics_;
    bool synced_ = false;
    int activeLine_ = -1;
    gfx::Text lyric_, lyricOld_;
    ui::Anim lyricIn_{1.f};

    // Controls.
    ui::Widget* drag_;               // the top strip moves the window (not in full screen)
    std::array<ui::Button*, kModes> modeBtns_{};
    ui::Button *lyricsBtn_, *fullBtn_, *close_;
    gfx::Text title_, artist_, time_, label_;
    Rect contentRect_{}, lyricRect_{}, cornerRect_{};

    CoverBackdrop backdrop_;
    // SHADETUBE_VIZ_LOG: frames and costs every 5 s.
    double statsAt_ = 0, paintMsSum_ = 0, paintMsMax_ = 0;
    int frames_ = 0, analysesAt_ = 0;
    gfx::DeviceHook deviceHook_{[this] { curtains_.Reset(); }};   // idle release: must not keep the device alive
};

} // namespace st::app
