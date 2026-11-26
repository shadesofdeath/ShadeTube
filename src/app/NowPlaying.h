#pragma once
// Full-screen "Şimdi Çalıyor" (docs/DEVELOPMENT.md, "Now Playing"). It covers the window below the title bar (the
// player bar steps aside while it is open) on a fluid backdrop in the cover's colors (app/CoverBackdrop).
//
//  - The track: the big cover (soft shadow, thin outline), the title (-> album) and the artists (each -> their page) in
//    large type with the like button, the match line ("Yanlış eşleşme?"; a station's codec, an episode's length), the
//    wave seek row and the player bar's thin outline controls (shuffle · prev · play · next · repeat), then sleep
//    timer, speed, listen together, volume knob and mini player.
//  - Tabs with a sliding indicator: Şarkı sözleri (a station: Canlı yayın, the titles it announced; none for a podcast
//    episode), Sıradaki (the queue panel) and Hakkında (album and artist from the catalog, the song's credits, an
//    episode's show notes, a station's details). The toolbar of the open tab sits at the right end of the tab row
//    (lyrics: Çeviri, the timing −/+, full screen, share); features add their buttons in buildToolbar(). The lyrics
//    header offers "Yanlış sözler mi?" (app/LyricsPicker).
//  - Wide windows: the track on the left, the tabs on the right. Narrow / short ones: a compact track header above the
//    tabs.
// A track change slides and fades the cover and the titles like the player bar (TrackSwap); lyrics and details fade in
// once they are there instead of flashing a placeholder. Everything it holds goes when it closes (deactivate()).
#include "app/Components.h"
#include "app/CoverBackdrop.h"
#include "app/LyricsService.h"
#include "catalog/Credits.h"
#include "lyrics/Lyrics.h"
#include "ui/Anim.h"
#include "ui/Controls.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace st::app {

class QueuePanel;
class NowPlayingTabs;   // the tab row (NowPlaying.cpp)
class VideoView;        // video mode (app/VideoView)

class NowPlayingView : public ui::Widget {
public:
    NowPlayingView();
    void activate();       // the view opens: sync, lyrics fetch for the current track
    void deactivate();     // release everything it holds (bitmaps, lyrics, details, text layouts)
    void onTrackChanged(); // player state / track / queue changed (App, while it is visible)

    enum class Tab { Lyrics, Queue, About };
    void showTab(Tab tab);
    // The queue shortcut while the view is open: Sıradaki, or back to the tab before it.
    void toggleQueueTab();
    // The view spans the whole window, its backdrop behind the title bar too: the top `inset` DIPs are the title bar's
    // (painted over the view by the Shell, hit-tested through it).
    void setTopInset(float inset);

    void layout() override;
    Widget* hitTest(gfx::Point pos) override;
    void paint(Canvas& c) override;
    bool onWheel(float dy, float, const ui::MouseEvent& e) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override;

private:
    // ---- what is playing ------------------------------------------------------------------------------------------
    enum class Kind { None, Song, Episode, Station };
    Kind kind() const;
    void syncTrackText();                // title / artist line / their link ranges for the current item
    void syncControls();                 // transport, like, speed, sleep, jam, volume from the player
    void paintTrack(Canvas& c);          // cover, titles, match line, times (with the track-change layers)
    void paintCover(Canvas& c, const Rect& r, bool outgoing);
    void paintArtShadow(Canvas& c, const Rect& art);
    void openTitle();                    // the title's link: album / show / radio page
    void leaveTo(const std::function<void()>& go);   // closes the view, then goes there

    // ---- tabs and their content -----------------------------------------------------------------------------------
    std::vector<Tab> availableTabs() const;
    Tab effectiveTab() const;            // the chosen one, or the first available
    void syncTabs();                     // labels / visibility for the item; selects effectiveTab()
    void buildToolbar();
    void layoutToolbar(const Rect& row, float leftLimit);
    // Lyrics.
    enum class LyricsState { None, Loading, Synced, Plain, Instrumental, Missing };
    void fetchLyrics();
    void syncLyricsControls();           // toolbar buttons for the lyrics state + the track's offset
    void rebuildTranslation();           // translated lines under the lyrics, for translation_'s state
    float translationHeight(int i, float width);   // with its gap above; 0 = nothing under line i
    float lineHeight(int i, float width);
    int lineAt(gfx::Point p) const;
    void paintLyrics(Canvas& c, const Rect& r);
    void paintLive(Canvas& c, const Rect& r);      // internet radio: heard titles instead of lyrics
    // Hakkında.
    void fetchAbout();
    void paintAbout(Canvas& c, const Rect& r);
    float paintAboutSong(Canvas& c, float x, float y, float w);      // returns the height used
    float paintAboutEpisode(Canvas& c, float x, float y, float w, float h);
    float paintAboutStation(Canvas& c, float x, float y, float w);
    float paintLink(Canvas& c, const std::wstring& text, float x, float y, std::function<void()> go);   // returns width

    // ---- layout ---------------------------------------------------------------------------------------------------
    bool wide_ = true;                   // track left / tabs right; else the compact header above the tabs
    float topInset_ = 0;                 // the title bar's height (setTopInset)
    Rect artRect_{}, infoRect_{};        // the cover; the text column under (wide) / beside (narrow) it
    Rect titleRect_{}, subRect_{}, metaRect_{}, matchRect_{};
    // Tab stops for the painted "Yanlış eşleşme?" / "Yanlış sözler mi?" links (placed at their rects every frame).
    ui::FocusProxy *matchFocus_ = nullptr, *pickerFocus_ = nullptr;
    Rect elapsedRect_{}, totalRect_{};
    Rect columnRect_{};                  // the tabs' column
    Rect contentRect_{};                 // below the tab row (and the label row)
    float titleSize_ = 36;

    // ---- widgets --------------------------------------------------------------------------------------------------
    ui::Slider* seek_;
    ui::Button *shuffle_, *prev_, *next_, *repeat_, *heart_;
    ui::PlayButton* play_;
    ui::Button *sleep_, *speed_, *jam_, *jamLabel_, *mini_;
    ui::Button* videoBtn_;               // video mode's toggle (songs only), beside the sleep timer
    VideoView* video_;                   // video mode's picture over the cover (app/VideoView)
    ui::Knob* knob_;
    NowPlayingTabs* tabs_;
    QueuePanel* queue_;
    ui::Button *translate_, *earlier_, *offset_, *later_, *fullscreen_;
    ui::Button* share_;                  // "Sözleri paylaş" (app/LyricsShare)
    // The toolbar on the right of the tab row, in this order (the first is leftmost). An entry belongs to one tab (or
    // to every tab: tab = nullopt) and shows while `shown` says so. Features add theirs in buildToolbar().
    struct ToolbarItem {
        ui::Widget* widget;
        std::optional<Tab> tab;
        std::function<bool()> shown;     // nullptr = always (on its tab)
        float gapAfter = 6;
    };
    std::vector<ToolbarItem> toolbar_;
    bool toolbarBelow_ = false;          // no room beside the tabs: on the label row below them
    float toolbarLeft_ = 0;              // its left edge (the label row's text stops there)
    std::wstring speedLabel_, jamText_, offsetText_;   // what the buttons show (labels are set only on a change)
    std::string likedId_;                // the track the heart showed at the last sync and whether it was liked: a
    bool likedWas_ = false;              // change of the same track plays the like feedback

    // ---- state ----------------------------------------------------------------------------------------------------
    Lifetime life_;                      // per track (details / credits requests)
    Lifetime lyricsLife_;                // the lyrics request (also renewed when the lyrics choice changes)
    Lifetime subs_;                      // library / listen-together subscriptions (the view's lifetime)
    std::string trackId_;
    Tab chosen_ = Tab::Lyrics;           // what the user picked (kept across tracks); effectiveTab() may differ
    Tab beforeQueue_ = Tab::Lyrics;      // toggleQueueTab() goes back here
    Tab shownTab_ = Tab::Lyrics;
    ui::Anim tabIn_{1.f};                // the content fading in after a tab switch
    ui::Anim lyricsIn_{1.f};             // a new track's lyrics fading in once they are there
    ui::Anim aboutIn_{1.f};              // Hakkında brightening once the catalog details arrive
    bool open_ = false;                  // activate() ran, deactivate() not yet

    // Track text (and the outgoing track's during a change).
    gfx::Text title_, sub_, meta_, elapsed_, total_;
    gfx::Text oldTitle_, oldSub_;
    struct Shown {
        std::vector<catalog::Image> images;
        Kind kind = Kind::None;
    };
    Shown shown_, old_;
    TrackSwap swap_;
    FadingArt art_;
    struct Range {
        uint32_t from = 0, len = 0;
        std::function<void()> go;
    };
    std::vector<Range> subLinks_;        // artists (and the album) in the artist line
    std::vector<Rect> subLinkRects_;     // as last painted, one per subLinks_ entry (empty rect = not shown)
    int hoverSub_ = -1;
    bool hoverTitle_ = false, hoverMatch_ = false;

    // Lyrics (as before the redesign).
    LyricsState state_ = LyricsState::None;
    double loadingSince_ = 0;
    bool askedSpotify_ = false;          // the "no lyrics" note names the sources asked
    lyrics::Choice pick_;                // the user's pick for this track ("Yanlış sözler mi?", app/LyricsPicker)
    Rect pickerRect_{};                  // the picker's link as last painted (empty = not shown)
    bool hoverPicker_ = false;
    lyrics::Lyrics lyrics_;
    std::vector<gfx::Text> lines_;       // inactive style; a line keeps its layout only while it is on screen
    std::vector<float> lineH_;           // measured heights (-1 = not yet) for width lineHW_
    float lineHW_ = -1;
    LyricsTranslation translation_;      // "Çeviri"
    std::vector<gfx::Text> trans_;       // each line's translation while shown (empty text = none), like lines_
    std::vector<float> transH_;
    float transHW_ = -1;
    gfx::Text activeText_;
    int active_ = -2;
    ui::SmoothScroll scroll_;            // follows the active line; the wheel takes over for 3 s (manualUntil_)
    double manualUntil_ = 0;
    std::vector<Rect> lineRects_;        // last painted (for hit testing)
    int hoverLine_ = -1;
    gfx::Text liveNow_;                  // internet radio: the song on air in the heard-titles list

    // Hakkında: fetched once per track while the tab is open; the track's own fields show meanwhile.
    struct About {
        enum class State { None, Loading, Ready, Failed } state = State::None;
        std::string trackId;
        std::optional<catalog::Album> album;   // without its tracks (count kept)
        std::optional<catalog::Artist> artist;
        std::vector<catalog::Artist> similar;
        // Song credits (app/Credits), fetched alongside: Loading, then Ready (credits set; empty = none known) or
        // Failed.
        State creditsState = State::None;
        std::optional<catalog::Credits> credits;
        std::vector<catalog::Credit> summary;   // the few people shown here (creditsSummary)
    };
    void fetchCredits();
    float paintAboutCredits(Canvas& c, float x, float y, float w);   // returns the height used
    About about_;
    ui::SmoothScroll aboutScroll_;
    float aboutH_ = 0;                   // content height at the last paint
    struct Link {
        Rect r;
        std::function<void()> go;
    };
    std::vector<Link> links_;            // Hakkında's links as last painted
    int hoverLink_ = -1;
    gfx::Text aboutTitle_, aboutName_;
    std::vector<std::pair<std::wstring, gfx::Text>> linkTexts_;   // "Albüme git"... (laid out once)
    std::vector<gfx::Text> chipTexts_;   // the similar artists' names
    std::vector<std::pair<gfx::Text, gfx::Text>> creditTexts_;   // the credits summary: name, roles

    CoverBackdrop backdrop_;
    // Hero art shadow, computed once on the CPU (see paintArtShadow).
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> shadow_;
    float shadowArt_ = 0;
    uint64_t shadowGen_ = 0, shadowTheme_ = 0;
    double openedAt_ = 0;                // ui::frame::realNow() at activate()
    bool trimmed_ = true;                // the settle trim ran for this opening (see paint)
    double statsAt_ = 0;                 // SHADETUBE_NP_LOG
    int framesSinceStats_ = 0;
    gfx::DeviceHook deviceHook_{[this] { shadow_.Reset(); }};   // idle release: must not keep the device alive
};

} // namespace st::app
