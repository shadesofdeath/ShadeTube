#include "app/NowPlaying.h"

#include "app/Commands.h"
#include "app/Credits.h"
#include "app/CreditsUi.h"
#include "app/InternetRadio.h"
#include "app/LyricsFullscreen.h"
#include "app/LyricsPicker.h"
#include "app/LyricsService.h"
#include "app/LyricsShare.h"
#include "app/LyricsSync.h"
#include "app/PodcastUi.h"
#include "app/Router.h"
#include "app/Shell.h"
#include "app/SmartShuffle.h"
#include "app/Source.h"
#include "app/Together.h"
#include "app/VideoMode.h"
#include "app/VideoView.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <shellapi.h>

#include <cctype>
#include <cmath>
#include <ctime>
#include <numbers>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;
using Tab = NowPlayingView::Tab;

namespace {

// A line's translation under it: smaller and dimmer than the inactive lines.
constexpr gfx::TextStyle kTranslationStyle{19, 500, 1.2f, -0.01f};
constexpr float kTranslationGap = 6;
constexpr float kTabRowH = 40;      // the tab row (tabs + toolbar)
constexpr float kTabGap = 24;       // between the tab row's labels
constexpr float kAboutArt = 76;     // Hakkında: album / artist / show pictures
constexpr float kSwapShift = 1.6f;  // the player bar's track-change slide (10 DIP), a bit longer for the big cover

// Catalog ids the detail lookups know: Spotify URIs and MusicBrainz MBIDs (local files, previews and imports have none).
bool catalogId(const std::string& id) {
    if (id.rfind("spotify:", 0) == 0) return true;
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(id[i]);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!std::isxdigit(ch)) {
            return false;
        }
    }
    return true;
}

// Dev: SHADETUBE_NP_LOG=1 logs the view's frame and backdrop numbers every 5 s while it is open.
bool devLog() {
    static const bool on = GetEnvironmentVariableW(L"SHADETUBE_NP_LOG", nullptr, 0) > 0;
    return on;
}

// What the details lookup brings back (worker -> UI thread).
struct AboutFetch {
    std::optional<catalog::Album> album;
    std::optional<catalog::Artist> artist;
    std::vector<catalog::Artist> similar;
    bool failed = false;
};

// A label of the tab row: fg.secondary -> fg.primary on hover / when selected. The row draws the sliding indicator.
class TabButton : public Button {
public:
    TabButton() : Button(ButtonKind::Tab) {}
    void paint(Canvas& c) override {
        const auto& col = colors();
        const Color fg = Color::lerp(Color::lerp(col.fgSecondary, col.fgPrimary, hover_.value()), col.fgPrimary, activeA_.value());
        const Rect r = rect();
        c.text(label_, {r.x, r.y, r.w + 1, r.h}, fg, gfx::VAlign::Center);
    }
};

// Smoothstep of x in [0, 1].
float smooth(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3 - 2 * x);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The tab row: Şarkı sözleri / Sıradaki / Hakkında with a 2 DIP accent indicator that slides to the selected label.

class NowPlayingTabs : public ui::Widget {
public:
    NowPlayingTabs() {
        hitTestVisible = false;   // the labels take the clicks
        for (int i = 0; i < 3; ++i) {
            auto* b = add<TabButton>();
            b->onClick = [this, i] {
                if (onSelect) {
                    const auto select = onSelect;   // may relayout this row
                    select(static_cast<Tab>(i));
                }
            };
            buttons_[i] = b;
        }
    }
    std::function<void(Tab)> onSelect;

    void setLabel(Tab t, const std::wstring& s) {
        auto& cur = labels_[static_cast<int>(t)];
        if (cur == s) return;
        cur = s;
        buttons_[static_cast<int>(t)]->setLabel(s);
    }
    void setShown(Tab t, bool on) { buttons_[static_cast<int>(t)]->setVisible(on); }
    void select(Tab t, bool animate) {
        const int i = static_cast<int>(t);
        for (int k = 0; k < 3; ++k) buttons_[k]->setActive(k == i);
        if (i == selected_ && placed_) return;
        from_ = indicator();
        selected_ = i;
        if (animate && placed_) {
            slide_.snap(0);
            slide_.to(1, ui::motion::medium, ui::Ease::Standard);
        } else {
            slide_.snap(1);
        }
        invalidate();
    }
    float naturalWidth() {
        float w = 0;
        for (auto* b : buttons_)
            if (b->visible()) w += std::ceil(b->naturalWidth()) + kTabGap;
        return std::max(0.f, w - kTabGap);
    }
    void layout() override {
        float x = 0;
        for (auto* b : buttons_) {
            if (!b->visible()) continue;
            const float w = std::ceil(b->naturalWidth());
            b->setRect({x, 0, w, rect().h});
            x += w + kTabGap;
        }
        if (!slide_.running()) from_ = target();
        placed_ = true;
    }
    void paint(Canvas& c) override {
        paintChildren(c);
        const Rect ind = indicator();
        if (ind.w > 0) c.fillRect({rect().x + ind.x, rect().bottom() - 2, ind.w, 2}, accent().base);
    }

private:
    Rect target() const {
        const auto* b = buttons_[selected_];
        return b->visible() ? b->rect() : Rect{};
    }
    Rect indicator() const {
        const Rect to = target();
        const float p = slide_.value();
        if (p >= 1.f || from_.w <= 0) return to;
        return {ui::lerp(from_.x, to.x, p), 0, ui::lerp(from_.w, to.w, p), 0};
    }
    Button* buttons_[3]{};
    std::wstring labels_[3];
    int selected_ = 0;
    bool placed_ = false;
    Rect from_{};
    ui::Anim slide_{1.f};
};

// ---------------------------------------------------------------------------------------------------------------------

NowPlayingView::NowPlayingView()
    : title_({}, type::displayS), sub_({}, type::bodyL.withSize(17)), meta_({}, type::monoMeta), elapsed_({}, type::monoMeta),
      total_({}, type::monoMeta), oldTitle_({}, type::displayS), oldSub_({}, type::bodyL.withSize(17)), liveNow_({}, type::title),
      aboutTitle_({}, type::sectionTitle), aboutName_({}, type::sectionTitle) {
    gfx::TextOptions wrap2;
    wrap2.wrap = true;
    wrap2.maxLines = 2;
    title_.setOptions(wrap2);
    oldTitle_.setOptions(wrap2);
    aboutTitle_.setOptions(wrap2);
    aboutName_.setOptions(wrap2);
    liveNow_.setOptions(wrap2);
    gfx::TextOptions right;
    right.align = gfx::TextAlign::Trailing;
    elapsed_.setOptions(right);

    // ---- controls (the player bar's, in the same order for the keyboard) ----
    // Tab order follows the reading order: the heart beside the title, "Yanlış eşleşme?" under it, then the seek row, the
    // transport and the utilities.
    heart_ = add<Button>(ButtonKind::Icon, L"", "heart");
    heart_->setTooltip(commands::keyTip(tr(L"Beğen"), "like"));
    heart_->setHeart("heart", "heart-filled");   // the like feedback (ui::HeartPop), as in the player bar
    heart_->onClick = [this] {
        const auto* t = ctx().player->current();
        if (!t) return;
        if (radio::isStationId(t->id)) {   // a station: the heart is the radio favorite
            if (const auto* s = radio::store().find(radio::uuidOf(t->id))) radio::store().toggleFavorite(*s);
        } else {
            ctx().library.toggleLiked(*t);
        }
        syncControls();
    };
    // "Yanlış eşleşme?" under the title: painted by this view, a Tab stop through its proxy (no tooltip: its text is
    // on screen).
    matchFocus_ = add<ui::FocusProxy>();
    matchFocus_->onActivateFn = [] {
        if (const auto* t = ctx().player ? ctx().player->current() : nullptr; t && ctx().showMatchPicker) ctx().showMatchPicker(*t);
    };
    matchFocus_->setVisible(false);
    seek_ = add<ui::Slider>(ui::Slider::Look::Wave);
    seek_->hitPadding = 6;
    seek_->onCommit = [](float f) { ctx().player->seek(static_cast<int64_t>(f * ctx().player->durationMs())); };
    shuffle_ = add<Button>(ButtonKind::Icon, L"", "shuffle");
    prev_ = add<Button>(ButtonKind::Icon, L"", "prev");
    play_ = add<ui::PlayButton>(ui::PlayButton::Look::Outline);
    next_ = add<Button>(ButtonKind::Icon, L"", "next");
    repeat_ = add<Button>(ButtonKind::Icon, L"", "repeat");
    shuffle_->setTooltip(commands::keyTip(tr(L"Karıştır"), "shuffle"));
    prev_->setTooltip(commands::keyTip(tr(L"Önceki"), "previous"));
    next_->setTooltip(commands::keyTip(tr(L"Sonraki"), "next"));
    repeat_->setTooltip(commands::keyTip(tr(L"Tekrarla"), "repeat"));
    shuffle_->onClick = [] { smartshuffle::cycleMode(); };   // off -> shuffle -> smart shuffle
    prev_->onClick = [] { ctx().player->previous(); };
    next_->onClick = [] { ctx().player->next(); };
    play_->onClick = [] { ctx().player->togglePause(); };
    repeat_->onClick = [] { ctx().player->cycleRepeat(); };
    sleep_ = add<Button>(ButtonKind::Icon, L"", "clock");
    sleep_->onClick = [this] {
        const Rect br = sleep_->toWindow(sleep_->rect());
        showSleepTimerMenu({br.x, br.y - 6});
    };
    // Video mode (app/VideoMode): the song's music video takes the cover's place (video_); the player bar's button is
    // hidden while the view is open, so the view has its own.
    videoBtn_ = add<Button>(ButtonKind::Icon, L"", "video");
    videoBtn_->onClick = [] { videomode::toggle(); };
    videoBtn_->onContext = [](const ui::MouseEvent& e) { videomode::showMenu(ctx().window, e.windowPos); };   // video mode's menu
    // Playback speed: always offered for podcast episodes, for songs once it isn't 1x (keys / palette set it).
    speed_ = add<Button>(ButtonKind::Ghost, speedLabel(1.f));
    speed_->setTooltip(tr(L"Çalma hızı"));
    speed_->onClick = [this] {
        auto* p = ctx().player;
        if (!p || !p->current()) return;
        std::vector<ui::MenuItem> items;
        for (float s : speedSteps()) {
            ui::MenuItem it{speedLabel(s), "", s == 1.f ? std::wstring(tr(L"Normal")) : std::wstring{}, [s] {
                                if (ctx().player) ctx().player->setSpeed(s);
                            }};
            it.checked = std::fabs(p->speed() - s) < 0.01f;
            items.push_back(std::move(it));
        }
        const Rect br = speed_->toWindow(speed_->rect());
        ui::Menu::open(ctx().window, {br.x, br.y - 6}, std::move(items));
    };
    // "Birlikte dinle": the icon, or during a session the pill that names it (both open the panel, which leaves
    // Now Playing).
    jam_ = add<Button>(ButtonKind::Icon, L"", "users");
    jam_->setTooltip(tr(L"Birlikte dinle"));
    jam_->onClick = [] { commands::run("listen-together"); };
    jamLabel_ = add<Button>(ButtonKind::Ghost, L"", "users");
    jamLabel_->setTooltip(tr(L"Birlikte dinle"));
    jamLabel_->onClick = [] { commands::run("listen-together"); };
    jamLabel_->setVisible(false);
    knob_ = add<ui::Knob>();
    knob_->setValue(Settings::get().volume);
    knob_->onChange = [](float v) { ctx().player->setVolume(v); };
    mini_ = add<Button>(ButtonKind::Icon, L"", "mini-player");
    mini_->setTooltip(commands::keyTip(tr(L"Mini oynatıcı"), "mini-player"));
    mini_->onClick = [] {
        if (ctx().openMiniPlayer) ctx().openMiniPlayer();
    };

    video_ = add<VideoView>(VideoView::Style::Inline);   // laid over the cover (layout); paints after the track block

    // ---- tabs, their toolbar and the queue ----
    tabs_ = add<NowPlayingTabs>();
    tabs_->setLabel(Tab::Lyrics, tr(L"Şarkı sözleri"));
    tabs_->setLabel(Tab::Queue, tr(L"Sıradaki"));
    tabs_->setLabel(Tab::About, tr(L"Hakkında"));
    tabs_->onSelect = [this](Tab t) { showTab(t); };
    buildToolbar();
    pickerFocus_ = add<ui::FocusProxy>();   // the painted link beside the lyrics label (its text is visible: no tooltip)
    pickerFocus_->onActivateFn = [this] {
        if (const auto* t = ctx().player ? ctx().player->current() : nullptr) showLyricsPicker(*t, lyrics_.lrclibId);
    };
    pickerFocus_->setVisible(false);
    queue_ = add<QueuePanel>();
    queue_->setEmbedded(true);
    queue_->setVisible(false);
    translation_.onChange = [this] {
        rebuildTranslation();
        syncLyricsControls();
    };
    syncLyricsControls();

    // Like state (the library) and the listen-together session change outside the player's own notifications.
    ctx().library.subscribe(subs_.ref(), [this] {
        if (open_) syncControls();
    });
    together::subscribe(subs_.ref(), [this] {
        if (open_) syncControls();
    });
    // Another lyrics pick for the track on screen ("Yanlış sözler mi?"): load them again.
    onLyricsChoiceChanged(subs_.ref(), [this](const std::string& id) {
        if (!open_ || id != trackId_) return;
        lyricsIn_.snap(0);
        fetchLyrics();
    });

    // Dev: SHADETUBE_NP_TAB=lyrics|queue|about opens on that tab (screenshots).
    wchar_t dev[16] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_NP_TAB", dev, 16) > 0) {
        const std::wstring_view v(dev);
        chosen_ = v == L"queue" ? Tab::Queue : v == L"about" ? Tab::About : Tab::Lyrics;
    }
}

// The toolbar on the right of the tab row. Left to right; each entry names its tab (nullopt = every tab) and when it
// shows. A feature's button goes here (created with add<>, then one entry).
void NowPlayingView::buildToolbar() {
    // Lyrics: [Çeviri]  [−] [+0,5 sn] [+]  [full screen] [share].
    translate_ = add<Button>(ButtonKind::Chip, tr(L"Çeviri"));
    translate_->setTooltip(tr(L"Her satırın çevirisini altında göster"));
    translate_->onClick = [this] { translation_.toggle(); };
    earlier_ = add<Button>(ButtonKind::Icon, L"", "minus");
    earlier_->setTooltip(commands::keyTip(tr(L"Sözleri daha erken göster"), "lyrics-earlier"));
    earlier_->onClick = [this] {
        setLyricsOffset(trackId_, lyricsOffset(trackId_) - kLyricsOffsetStepMs);
        syncLyricsControls();
    };
    offset_ = add<Button>(ButtonKind::Ghost, formatLyricsOffset(0));
    offset_->setTooltip(tr(L"Söz zamanlamasını sıfırla"));
    offset_->onClick = [this] {
        setLyricsOffset(trackId_, 0);
        syncLyricsControls();
    };
    later_ = add<Button>(ButtonKind::Icon, L"", "plus");
    later_->setTooltip(commands::keyTip(tr(L"Sözleri daha geç göster"), "lyrics-later"));
    later_->onClick = [this] {
        setLyricsOffset(trackId_, lyricsOffset(trackId_) + kLyricsOffsetStepMs);
        syncLyricsControls();
    };
    fullscreen_ = add<Button>(ButtonKind::Icon, L"", "expand");
    fullscreen_->setTooltip(commands::keyTip(tr(L"Tam ekran sözler"), "lyrics-fullscreen"));
    fullscreen_->onClick = [] {
        if (ctx().toggleLyricsFullscreen) ctx().toggleLyricsFullscreen();
    };
    // "Sözleri paylaş" (app/LyricsShare): a card of the lines around the sung one.
    share_ = add<Button>(ButtonKind::Icon, L"", "share");
    share_->setTooltip(tr(L"Sözleri paylaş"));
    share_->onClick = [this] { openLyricsShare(lyrics_, active_, translation_); };
    // Plain lyrics: time them by tapping along (app/LyricsSync).
    syncLyrics_ = add<Button>(ButtonKind::Chip, tr(L"Senkronla"));
    syncLyrics_->setTooltip(tr(L"Sözleri şarkıyla birlikte kendin zamanla (söz senkronlayıcı)"));
    syncLyrics_->onClick = [] {
        if (const auto* t = ctx().player ? ctx().player->current() : nullptr) showLyricsSync(*t);
    };
    auto synced = [this] { return state_ == LyricsState::Synced; };
    auto hasText = [this] { return state_ == LyricsState::Synced || state_ == LyricsState::Plain; };
    toolbar_ = {
        {syncLyrics_, Tab::Lyrics, [this] { return state_ == LyricsState::Plain && kind() == Kind::Song; }, 12},
        {translate_, Tab::Lyrics, [this, hasText] { return hasText() && translation_.offered(); }, 12},
        {earlier_, Tab::Lyrics, synced, 2},
        {offset_, Tab::Lyrics, synced, 2},
        {later_, Tab::Lyrics, synced, 12},
        {fullscreen_, Tab::Lyrics, hasText, 6},
        {share_, Tab::Lyrics, hasText, 0},
    };
}

NowPlayingView::Kind NowPlayingView::kind() const {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t) return Kind::None;
    if (radio::isStationId(t->id)) return Kind::Station;
    if (catalog::isPodcastId(t->id)) return Kind::Episode;
    return Kind::Song;
}

void NowPlayingView::activate() {
    open_ = true;
    swap_ = TrackSwap{};   // opening shows the current track in place
    onTrackChanged();
    syncTabs();
    queue_->sync();
    openedAt_ = ui::frame::realNow();
    trimmed_ = false;
    statsAt_ = openedAt_;
    framesSinceStats_ = 0;
}

void NowPlayingView::deactivate() {
    open_ = false;
    backdrop_.release();
    shadow_.Reset();
    lines_.clear();
    lines_.shrink_to_fit();
    lineH_.clear();
    lineH_.shrink_to_fit();
    lineRects_.clear();
    lineRects_.shrink_to_fit();
    lyrics_ = {};
    activeText_ = {};
    active_ = -2;
    translation_.clear();
    trans_.clear();
    trans_.shrink_to_fit();
    transH_.clear();
    transH_.shrink_to_fit();
    about_ = {};
    links_.clear();
    links_.shrink_to_fit();
    subLinkRects_.clear();
    // The display-size layouts are rebuilt on the next open.
    for (auto* t : {&title_, &sub_, &meta_, &oldTitle_, &oldSub_, &liveNow_, &aboutTitle_, &aboutName_, &elapsed_, &total_})
        t->reset();
    trackId_.clear();
    state_ = LyricsState::None;
    syncLyricsControls();
    life_.renew();
    lyricsLife_.renew();
    creditTexts_.clear();
    creditTexts_.shrink_to_fit();
    // Video mode: its picture goes with the view (a pop-out window keeps it); posted: the view is hidden after this.
    Dispatcher::post([] { videomode::sync(); });
}

void NowPlayingView::syncTrackText() {
    const auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    subLinks_.clear();
    if (!t) return;
    if (radio::isStationId(t->id)) {
        // A station: the song it announces (ICY "Artist - Title") is the big title, the station below it.
        const std::wstring& song = p->liveTitle();
        title_.setText(song.empty() ? toWide(t->name) : song);
        sub_.setText(song.empty() ? toWide(t->artistLine()) : toWide(t->name));
        return;
    }
    if (catalog::isPodcastId(t->id)) {   // an episode: its show once (-> the show's page)
        const std::wstring show = toWide(t->album.name.empty() ? t->artistLine() : t->album.name);
        title_.setText(toWide(t->name));
        sub_.setText(show);
        const catalog::Track episode = *t;
        subLinks_.push_back({0, static_cast<uint32_t>(show.size()), [episode] { openEpisodeShow(episode); }});
        return;
    }
    // A song: "Artist A, Artist B · Album", each artist with an id -> its page, the album -> its page.
    title_.setText(toWide(t->name));
    std::wstring s;
    for (size_t i = 0; i < t->artists.size(); ++i) {
        if (i) s += L", ";
        const auto from = static_cast<uint32_t>(s.size());
        s += toWide(t->artists[i].name);
        if (const std::string id = t->artists[i].id; !id.empty())
            subLinks_.push_back({from, static_cast<uint32_t>(s.size()) - from, [id] { ctx().router->navigate({RouteKind::Artist, id}); }});
    }
    if (!t->album.name.empty()) {
        if (!s.empty()) s += L" · ";
        const auto from = static_cast<uint32_t>(s.size());
        s += toWide(t->album.name);
        if (const std::string id = t->album.id; !id.empty())
            subLinks_.push_back({from, static_cast<uint32_t>(s.size()) - from, [id] { ctx().router->navigate({RouteKind::Album, id}); }});
    }
    sub_.setText(s);
}

void NowPlayingView::onTrackChanged() {
    auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    syncControls();
    if (!t) return;
    // A new track: what was shown so far becomes the outgoing layer of the transition.
    if (swap_.update(p)) {
        old_ = shown_;
        oldTitle_.setStyle(title_.style());
        oldTitle_.setText(title_.text());
        oldSub_.setStyle(sub_.style());
        oldSub_.setText(sub_.text());
    }
    shown_.images = t->album.images;
    shown_.kind = kind();
    syncTrackText();
    if (t->id != trackId_ && visible()) {
        trackId_ = t->id;
        life_.renew();       // the last track's details / credits requests
        lyricsIn_.snap(0);   // the new lyrics fade in once they are there
        fetchLyrics();
        about_ = {};
        aboutScroll_.snap(0);
        syncTabs();
        if (effectiveTab() == Tab::About) fetchAbout();
    }
    queue_->sync();
    requestLayout();
    invalidate();
}

void NowPlayingView::syncControls() {
    auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    const bool live = t && radio::isStationId(t->id);
    play_->setPlaying(p && p->isPlaying());
    play_->setLoading(p && p->status() == player::Status::Resolving);
    play_->setTooltip(commands::keyTip(p && p->isPlaying() ? tr(L"Duraklat") : tr(L"Oynat"), "play-pause"));
    seek_->setFlowing(p && p->status() == player::Status::Playing);
    seek_->setLive(live);
    const auto shuffleMode = smartshuffle::mode();
    shuffle_->setActive(shuffleMode != smartshuffle::Mode::Off);
    shuffle_->setIcon(shuffleMode == smartshuffle::Mode::Smart ? "shuffle-smart" : "shuffle");
    shuffle_->setTooltip(commands::keyTip(smartshuffle::modeLabel(shuffleMode), "shuffle"));
    const auto rep = p ? p->repeat() : RepeatMode::Off;
    repeat_->setActive(rep != RepeatMode::Off);
    repeat_->setIcon(rep == RepeatMode::One ? "repeat-one" : "repeat");
    const bool liked = t && (live ? radio::store().isFavorite(radio::uuidOf(t->id)) : ctx().library.isLiked(t->id));
    heart_->setIcon(liked ? "heart-filled" : "heart");
    heart_->setActive(liked);
    // Liked / unliked while it plays (this heart, the shortcut, a menu, the phone): the feedback; another song just
    // shows its own state.
    if (const std::string id = t ? t->id : std::string(); !id.empty() && id == likedId_ && liked != likedWas_) heart_->pulse();
    likedId_ = t ? t->id : std::string();
    likedWas_ = liked;
    heart_->setTooltip(!live ? commands::keyTip(tr(L"Beğen"), "like") : liked ? std::wstring(tr(L"Favorilerden kaldır")) : std::wstring(tr(L"Favorilere ekle")));
    heart_->setVisible(t && (live || ctx().library.canLike(t->id)));
    {
        const float sp = p ? p->speed() : 1.f;
        const bool episode = t && catalog::isPodcastId(t->id);
        speed_->setVisible(t && !live && (episode || std::fabs(sp - 1.f) > 0.01f));
        const std::wstring label = speedLabel(sp);
        if (speed_->naturalWidth() <= 0 || label != speedLabel_) {
            speedLabel_ = label;
            speed_->setLabel(label);
        }
        speed_->setActive(std::fabs(sp - 1.f) > 0.01f);
    }
    sleep_->setActive(sleepTimerActive());
    sleep_->setTooltip(sleepTimerActive() ? i18n::format(tr(L"Uyku zamanlayıcı · {}"), {sleepTimerLabel()})
                                          : std::wstring(tr(L"Uyku zamanlayıcı")));
    videoBtn_->setVisible(videomode::offered());   // songs only, as in the player bar
    videoBtn_->setEnabled(videomode::enabled());
    videoBtn_->setActive(videomode::on());
    videoBtn_->setTooltip(!videomode::enabled() ? std::wstring(tr(L"Bu şarkı için video yok"))
                          : videomode::on()     ? commands::keyTip(tr(L"Video modunu kapat"), "video-mode")
                                                : commands::keyTip(tr(L"Video modu"), "video-mode"));
    for (auto* b : {prev_, next_}) b->setEnabled(t != nullptr);
    // Listen together: a guest's transport follows the host (requests when the host allows it, else off), shuffle and
    // repeat are the host's.
    if (const std::wstring session = together::barLabel(); session != jamText_) {
        jamText_ = session;
        jamLabel_->setLabel(session);
    }
    jam_->setActive(together::active());
    jamLabel_->setActive(together::active());
    if (together::guest()) {
        const bool control = together::guestCanControl();
        for (auto* b : {prev_, next_}) b->setEnabled(t && control);
        play_->setEnabled(t && control);
        seek_->setEnabled(!live && control);
        shuffle_->setEnabled(false);
        repeat_->setEnabled(false);
    } else {
        play_->setEnabled(true);
        seek_->setEnabled(!live);
        shuffle_->setEnabled(true);
        repeat_->setEnabled(true);
    }
    if (p) knob_->setValue(p->volume());
    const int64_t durMs = p ? p->durationMs() : 0;
    seek_->keyStep = durMs > 0 ? std::clamp(5000.f / static_cast<float>(durMs), 0.001f, 0.25f) : 0.f;
    requestLayout();
    invalidate();
}

// ---- tabs ------------------------------------------------------------------------------------------------------------

std::vector<Tab> NowPlayingView::availableTabs() const {
    if (kind() == Kind::Episode) return {Tab::Queue, Tab::About};   // an episode has no lyrics: its notes are Hakkında
    return {Tab::Lyrics, Tab::Queue, Tab::About};
}

Tab NowPlayingView::effectiveTab() const {
    const auto tabs = availableTabs();
    if (std::find(tabs.begin(), tabs.end(), chosen_) != tabs.end()) return chosen_;
    return kind() == Kind::Episode ? Tab::About : tabs.front();
}

void NowPlayingView::syncTabs() {
    const Kind k = kind();
    tabs_->setLabel(Tab::Lyrics, k == Kind::Station ? tr(L"Canlı yayın") : tr(L"Şarkı sözleri"));
    tabs_->setShown(Tab::Lyrics, k != Kind::Episode);
    const Tab eff = effectiveTab();
    if (eff != shownTab_) {
        shownTab_ = eff;
        if (open_ && !ui::motion::reduced()) {
            tabIn_.snap(0);
            tabIn_.to(1, ui::motion::medium, ui::Ease::Decelerate);
        }
    }
    tabs_->select(eff, open_);
    queue_->setVisible(eff == Tab::Queue);
    if (eff == Tab::About) fetchAbout();
    requestLayout();
    invalidate();
}

void NowPlayingView::showTab(Tab tab) {
    chosen_ = tab;
    syncTabs();
    if (tab != Tab::Lyrics) pickerFocus_->place({});   // the lyrics header's link is no Tab stop on the other tabs
}

void NowPlayingView::setTopInset(float inset) {
    if (inset == topInset_) return;
    topInset_ = inset;
    requestLayout();
}

ui::Widget* NowPlayingView::hitTest(gfx::Point pos) {
    if (pos.y < rect().y + topInset_) return nullptr;   // the title bar's strip
    return Widget::hitTest(pos);
}

void NowPlayingView::toggleQueueTab() {
    const Tab cur = effectiveTab();
    if (cur == Tab::Queue) {
        showTab(beforeQueue_ == Tab::Queue ? Tab::Lyrics : beforeQueue_);
        return;
    }
    beforeQueue_ = cur;
    showTab(Tab::Queue);
}

void NowPlayingView::layoutToolbar(const Rect& row, float leftLimit) {
    // Right-aligned on the tab row; when the tabs leave no room, on the row below (the label row).
    float total = 0;
    std::vector<std::pair<ToolbarItem*, float>> shown;
    for (auto& it : toolbar_) {
        const bool on = (!it.tab || *it.tab == shownTab_) && (!it.shown || it.shown());
        it.widget->setVisible(on);
        if (!on) continue;
        float w = 28;
        if (auto* b = dynamic_cast<Button*>(it.widget)) {
            if (b->kind() == ButtonKind::Chip) w = std::ceil(b->naturalWidth());
            else if (b->kind() == ButtonKind::Ghost || b->kind() == ButtonKind::Secondary) w = std::max(64.f, std::ceil(b->naturalWidth()));
        }
        shown.emplace_back(&it, w);
        total += w + it.gapAfter;
    }
    if (!shown.empty()) total -= shown.back().first->gapAfter;
    toolbarBelow_ = !shown.empty() && row.right() - total < leftLimit + 16;
    const float cy = toolbarBelow_ ? row.bottom() + 18 : row.cy();
    float x = row.right();
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
        if (it != shown.rbegin()) x -= it->first->gapAfter;
        x -= it->second;
        it->first->widget->setRect({x, cy - 14, it->second, 28});
    }
    toolbarLeft_ = shown.empty() ? row.right() : x;
}

// ---- layout ----------------------------------------------------------------------------------------------------------

void NowPlayingView::layout() {
    const Rect r = rect();
    const float w = r.w, h = r.h;
    const float padX = std::round(std::clamp(w * 0.045f, 24.f, 72.f));
    const float padTop = topInset_ + 20, padBottom = 28;
    wide_ = w >= 900 && h - topInset_ >= 520;
    // Rows under the title: artist line, match line, seek row, transport, utilities.
    constexpr float kSubGap = 6, kSubH = 26, kMetaGap = 6, kMetaH = 18;
    auto subStyle = [](float titleSize) {
        return type::bodyL.withSize(std::round(std::clamp(titleSize * 0.46f, 15.f, 19.f))).withWeight(500);
    };
    float transport = 40, playSize = 52, gapT = 18;   // transport buttons
    float y = 0;
    Rect seekRow{}, transRow{}, utilRow{};
    if (wide_) {
        constexpr float kTitleGap = 26, kSeekGap = 24, kSeekH = 20, kTransGap = 14, kUtilGap = 14, kUtilH = 36;
        const float gap = std::round(std::clamp(w * 0.05f, 40.f, 96.f));
        const float leftW = std::round(std::clamp((w - 2 * padX - gap) * 0.44f, 340.f, 560.f));
        float cover = leftW, blockW = leftW, th = 0, fixed = 0;
        for (int pass = 0; pass < 2; ++pass) {
            titleSize_ = std::round(std::clamp(cover * 0.078f, 28.f, 42.f));
            title_.setStyle(type::displayS.withSize(titleSize_));
            sub_.setStyle(subStyle(titleSize_));
            th = std::ceil(title_.measure(blockW - 44).h);
            fixed = kTitleGap + th + kSubGap + kSubH + kMetaGap + kMetaH + kSeekGap + kSeekH + kTransGap + playSize + kUtilGap + kUtilH;
            cover = std::floor(std::clamp(h - padTop - padBottom - fixed, 140.f, leftW));
            blockW = std::clamp(cover, 340.f, leftW);
        }
        const float blockX = padX + std::round((leftW - blockW) * 0.5f);
        const float top = padTop + std::max(0.f, std::round((h - padTop - padBottom - cover - fixed) * 0.5f));
        artRect_ = {blockX + std::round((blockW - cover) * 0.5f), top, cover, cover};
        infoRect_ = {blockX, artRect_.bottom(), blockW, fixed};
        y = artRect_.bottom() + kTitleGap;
        titleRect_ = {blockX, y, blockW - 44, th};
        heart_->setRect({blockX + blockW - 36, y + std::round((std::min(th, titleSize_ * 1.05f) - 36) * 0.5f), 36, 36});
        y += th + kSubGap;
        subRect_ = {blockX, y, blockW, kSubH};
        y += kSubH + kMetaGap;
        metaRect_ = {blockX, y, blockW, kMetaH};
        y += kMetaH + kSeekGap;
        seekRow = {blockX, y, blockW, kSeekH};
        y += kSeekH + kTransGap;
        transRow = {blockX, y, blockW, playSize};
        y += playSize + kUtilGap;
        utilRow = {blockX, y, blockW, kUtilH};
        columnRect_ = {padX + leftW + gap, padTop, w - padX - (padX + leftW + gap), h - padTop - padBottom};
    } else {
        // The compact header: the cover on the left, the text and the controls beside it.
        transport = 36;
        playSize = 44;
        gapT = 10;
        constexpr float kSeekGap = 12, kSeekH = 20, kTransGap = 8, kUtilH = 32;
        const float cover = std::round(std::clamp(std::min((h - topInset_) * 0.3f, w * 0.26f), 96.f, 200.f));
        artRect_ = {padX, padTop, cover, cover};
        const float ix = artRect_.right() + 24, iw = std::max(160.f, w - padX - ix);
        titleSize_ = std::round(std::clamp(w * 0.03f, 22.f, 28.f));
        title_.setStyle(type::displayS.withSize(titleSize_));
        sub_.setStyle(subStyle(titleSize_));
        const float th = std::ceil(title_.measure(iw - 44).h);
        y = padTop;
        titleRect_ = {ix, y, iw - 44, th};
        heart_->setRect({ix + iw - 36, y + std::round((std::min(th, titleSize_ * 1.05f) - 36) * 0.5f), 36, 36});
        y += th + kSubGap - 2;
        subRect_ = {ix, y, iw, kSubH - 4};
        y += kSubH - 4 + kMetaGap - 2;
        metaRect_ = {ix, y, iw, kMetaH};
        y += kMetaH + kSeekGap;
        seekRow = {ix, y, iw, kSeekH};
        y += kSeekH + kTransGap;
        const float transportW = 4 * transport + playSize + 4 * gapT;
        const float utilW = 32 + 6 + 32 + 6 + 64 + 6 + 32 + 16 + 36 + 6 + 32;   // sleep, video, speed, jam | knob, mini
        if (iw >= transportW + 24 + utilW) {   // one row: transport on the left, the utilities on the right
            transRow = {ix, y, transportW, playSize};
            utilRow = {ix + transportW + 24, y + (playSize - kUtilH) * 0.5f, iw - transportW - 24, kUtilH};
            y += playSize;
        } else {
            transRow = {ix, y, transportW, playSize};
            y += playSize + 6;
            utilRow = {ix, y, iw, kUtilH};
            y += kUtilH;
        }
        infoRect_ = {ix, padTop, iw, y - padTop};
        const float headerB = std::max(artRect_.bottom(), y) + 20;
        columnRect_ = {padX, headerB, w - 2 * padX, std::max(120.f, h - headerB - padBottom)};
    }
    // Video mode's picture over the cover: on the cover's bottom edge next to the title (wide), centered beside the
    // text (compact).
    video_->setRect(artRect_);
    video_->setVerticalAlign(wide_ ? 1.f : 0.5f);
    // Seek row: elapsed · wave · total.
    elapsedRect_ = {seekRow.x, seekRow.y, 46, seekRow.h};
    totalRect_ = {seekRow.right() - 46, seekRow.y, 46, seekRow.h};
    seek_->setRect({seekRow.x + 46 + 12, seekRow.cy() - 8, seekRow.w - 2 * (46 + 12), 16});
    // Transport: centered under the cover (wide), from the left (compact).
    {
        const float total = 4 * transport + playSize + 4 * gapT;
        float x = wide_ ? std::round(transRow.cx() - total * 0.5f) : transRow.x;
        for (Widget* wd : std::initializer_list<Widget*>{shuffle_, prev_, play_, next_, repeat_}) {
            const float s = wd == play_ ? playSize : transport;
            wd->setRect({x, transRow.cy() - s * 0.5f, s, s});
            x += s + gapT;
        }
        for (auto* b : {shuffle_, prev_, next_, repeat_}) b->setIconSize(wide_ ? 20.f : 18.f);
    }
    // Utilities: sleep timer, speed, listen together on the left; volume knob and mini player on the right.
    {
        const float cy = utilRow.cy();
        float rx = utilRow.right();
        mini_->setRect({rx - 32, cy - 16, 32, 32});
        rx -= 32 + 8;
        knob_->setRect({rx - 36, cy - 18, 36, 36});
        rx -= 36 + 16;
        float lx = wide_ ? utilRow.x - 4 : utilRow.x;
        sleep_->setRect({lx, cy - 16, 32, 32});
        lx += 32 + 6;
        if (videoBtn_->visible()) {
            videoBtn_->setRect({lx, cy - 16, 32, 32});
            lx += 32 + 6;
        }
        if (speed_->visible()) {
            const float sw = std::max(44.f, std::ceil(speed_->naturalWidth()));
            speed_->setRect({lx, cy - 16, sw, 32});
            lx += sw + 6;
        }
        const float pillW = std::min(240.f, std::ceil(jamLabel_->naturalWidth()));
        const bool pill = together::active() && lx + pillW <= rx;
        jamLabel_->setVisible(pill);
        jam_->setVisible(!pill);
        if (pill) jamLabel_->setRect({lx, cy - 16, pillW, 32});
        else jam_->setRect({lx, cy - 16, 32, 32});
    }
    // The tab column: the tab row, its toolbar, the content below.
    const float tabsW = std::min(columnRect_.w, tabs_->naturalWidth());
    tabs_->setRect({columnRect_.x, columnRect_.y, tabsW, kTabRowH});
    layoutToolbar({columnRect_.x, columnRect_.y, columnRect_.w, kTabRowH}, columnRect_.x + tabsW);
    contentRect_ = {columnRect_.x, columnRect_.y + kTabRowH, columnRect_.w, std::max(0.f, columnRect_.h - kTabRowH)};
    // The queue's rows reach 20 DIP past the column on both sides (their hover fill), its text lines up with the tabs.
    queue_->setRect({contentRect_.x - 20, contentRect_.y + 14, contentRect_.w + 40, std::max(0.f, contentRect_.h - 14)});
}

// ---- lyrics ----------------------------------------------------------------------------------------------------------

void NowPlayingView::fetchLyrics() {
    lyricsLife_.renew();
    pick_ = {};
    lyrics_ = {};
    lines_.clear();
    lineH_.clear();
    lineRects_.clear();
    translation_.clear();
    trans_.clear();
    transH_.clear();
    active_ = -2;
    scroll_.snap(0);
    manualUntil_ = 0;
    const auto* t = ctx().player->current();
    if (t && (radio::isStationId(t->id) || catalog::isPodcastId(t->id))) {
        // Live radio: the heard titles take the lyrics' place (paintLive); an episode: its notes are in Hakkında.
        state_ = LyricsState::None;
        lyricsIn_.to(1, ui::motion::lyrics, ui::Ease::Decelerate);
        syncLyricsControls();
        return;
    }
    if (!t || !Settings::get().lyricsEnabled) {
        state_ = LyricsState::Missing;
        lyricsIn_.to(1, ui::motion::lyrics, ui::Ease::Decelerate);
        syncLyricsControls();
        return;
    }
    state_ = LyricsState::Loading;
    loadingSince_ = ui::frame::realNow();
    syncLyricsControls();
    // The user's pick, else the track's own file first (a .lrc next to it, its tags), then LRCLIB, then Spotify when
    // logged in.
    const lyrics::Query q = lyricsQueryFor(*t);
    pick_ = q.choice;
    askedSpotify_ = static_cast<bool>(q.fallback);
    async(Priority::High, lyricsLife_.ref(), [q] { return lyrics::fetch(q); },
          [this](Result<std::optional<lyrics::Lyrics>> r) {
              // Fade in from where the content stands now (a placeholder that already showed fades over).
              lyricsIn_.snap(0);
              lyricsIn_.to(1, ui::motion::lyrics, ui::Ease::Decelerate);
              if (!r || !*r) {
                  state_ = LyricsState::Missing;
                  syncLyricsControls();
                  invalidate();
                  return;
              }
              lyrics_ = std::move(**r);
              if (lyrics_.instrumental) state_ = LyricsState::Instrumental;
              else if (lyrics_.lines.empty()) state_ = LyricsState::Missing;
              else state_ = lyrics_.synced ? LyricsState::Synced : LyricsState::Plain;
              gfx::TextOptions wrap;
              wrap.wrap = true;
              lines_.clear();
              lines_.reserve(lyrics_.lines.size());
              const gfx::TextStyle inactive = wide_ ? type::lyricInactive : type::lyricInactive.withSize(22);
              for (const auto& l : lyrics_.lines) lines_.emplace_back(l.text.empty() ? L"♪" : toWide(l.text), inactive, wrap);
              lineH_.assign(lines_.size(), -1.f);
              activeText_ = gfx::Text({}, wide_ ? type::lyricActive : type::lyricActive.withSize(32), wrap);
              // Offered when the lyrics are in another language (found out off the UI thread; onChange follows).
              if (state_ == LyricsState::Synced || state_ == LyricsState::Plain) translation_.setLyrics(lyrics_, trackId_);
              syncLyricsControls();
              invalidate();
          });
}

void NowPlayingView::rebuildTranslation() {
    trans_.clear();
    transH_.clear();
    if (translation_.shown()) {
        gfx::TextOptions wrap;
        wrap.wrap = true;
        trans_.reserve(lines_.size());
        for (size_t i = 0; i < lines_.size(); ++i) trans_.emplace_back(toWide(translation_.line(i)), kTranslationStyle, wrap);
        transH_.assign(trans_.size(), -1.f);
    }
    invalidate();
}

float NowPlayingView::translationHeight(int i, float width) {
    if (i < 0 || i >= static_cast<int>(trans_.size()) || trans_[i].empty()) return 0;
    if (width != transHW_ || transH_.size() != trans_.size()) {
        transH_.assign(trans_.size(), -1.f);
        transHW_ = width;
    }
    if (transH_[i] < 0) {
        transH_[i] = trans_[i].measure(width).h;
        trans_[i].reset();
    }
    return kTranslationGap + transH_[i];
}

void NowPlayingView::syncLyricsControls() {
    translate_->setActive(translation_.on());
    const std::wstring label = formatLyricsOffset(lyricsOffset(trackId_));
    if (label != offsetText_) {
        offsetText_ = label;
        offset_->setLabel(label);
    }
    requestLayout();   // layoutToolbar() shows the buttons the state wants
    invalidate();
}

// Heights are cached per line; a line's layout is dropped right after measuring and rebuilt only while the line is on
// screen (a long song's wrapped 28 px layouts add up).
float NowPlayingView::lineHeight(int i, float width) {
    if (i == active_) {
        activeText_.setText(lines_[i].text());
        return activeText_.measure(width).h;
    }
    if (width != lineHW_ || lineH_.size() != lines_.size()) {
        lineH_.assign(lines_.size(), -1.f);
        lineHW_ = width;
    }
    if (lineH_[i] < 0) {
        lineH_[i] = lines_[i].measure(width).h;
        lines_[i].reset();
    }
    return lineH_[i];
}

void NowPlayingView::paintLyrics(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const Color label = backdrop_.labelColor();
    const float fade = lyricsIn_.value() * tabIn_.value();
    // Lyric sizes follow the layout (the compact header leaves less room).
    if (const gfx::TextStyle inactive = wide_ ? type::lyricInactive : type::lyricInactive.withSize(22);
        !lines_.empty() && !(lines_[0].style() == inactive)) {
        for (auto& l : lines_) l.setStyle(inactive);
        activeText_.setStyle(wide_ ? type::lyricActive : type::lyricActive.withSize(32));
        lineH_.assign(lines_.size(), -1.f);
    }
    std::wstring text = state_ == LyricsState::Synced ? tr(L"ŞARKI SÖZLERİ · SENKRONİZE") : toUpperTr(tr(L"Şarkı sözleri"));
    if ((state_ == LyricsState::Synced || state_ == LyricsState::Plain) && !lyrics_.source.empty())
        text += L" · " + lyricsSourceLabel(lyrics_.source);
    if (pick_.kind != lyrics::Choice::Kind::Auto && pick_.kind != lyrics::Choice::Kind::None &&
        (state_ == LyricsState::Synced || state_ == LyricsState::Plain || state_ == LyricsState::Instrumental))
        text += L" · " + toUpperTr(tr(L"Senin seçimin"));
    // The translation follows the setting / target language changed in the full-screen view or Ayarlar.
    if (state_ == LyricsState::Synced || state_ == LyricsState::Plain) translation_.sync();
    const float labelY = r.y + 16;
    const float labelRight = toolbarBelow_ ? toolbarLeft_ - 12 : r.right();
    const float avail = std::max(40.f, labelRight - r.x);
    // "Yanlış sözler mi?" right after the label (app/LyricsPicker), like "Yanlış eşleşme?" under the title; the label
    // gives way when the row is short.
    pickerRect_ = {};
    const bool lyricsShown = state_ == LyricsState::Synced || state_ == LyricsState::Plain || state_ == LyricsState::Instrumental;
    float labelW = avail;
    if (lyricsShown) {
        const std::wstring link = tr(L"Yanlış sözler mi?");
        auto ll = gfx::makeLayout(link, type::caption, 400);
        auto tl = gfx::makeLayout(text, type::monoLabel, 4000);
        DWRITE_TEXT_METRICS lm{}, tm{};
        ll->GetMetrics(&lm);
        tl->GetMetrics(&tm);
        const float lw = std::ceil(lm.widthIncludingTrailingWhitespace) + 4;
        const float fit = std::min(std::ceil(tm.widthIncludingTrailingWhitespace) + 1, avail - lw - 14);
        if (fit >= 60) {
            labelW = fit;
            pickerRect_ = {r.x + fit + 14, labelY - 3, lw, 22};
            c.text(link, type::caption, pickerRect_, (hoverPicker_ ? accent().base : col.fgSecondary).mulAlpha(tabIn_.value()),
                   gfx::TextAlign::Leading, gfx::VAlign::Center);
        }
    }
    c.text(text, type::monoLabel, {r.x, labelY, labelW, 16}, label);
    pickerFocus_->place(shownTab_ == Tab::Lyrics && kind() == Kind::Song ? pickerRect_ : Rect{});
    // The translation's state on the line below: "ÇEVRİLİYOR…", "ÇEVİRİ ALINAMADI", what is shown.
    if (const std::wstring note = translation_.note(); !note.empty())
        c.text(note, type::monoLabel, {r.x, labelY + 20, r.w, 16}, label);
    // The offset value flashes in the accent right after a change.
    if (offset_->visible() && lyricsOffsetChangedAt() > 0) {
        const double since = ui::frame::realNow() - lyricsOffsetChangedAt();
        if (since < 1200) {
            c.fillPill(offset_->rect(), accent().base.withAlpha(0.18f * static_cast<float>(1.0 - since / 1200)));
            ui::frame::requestNext();
        }
    }
    const float top = labelY + 44;
    const Rect area{r.x, top, r.w, std::max(0.f, r.bottom() - top)};
    auto centered = [&](const wchar_t* big, const wchar_t* note) {
        c.text(big, type::headline, {area.x, area.cy() - 40, area.w, 34}, col.fgSecondary.mulAlpha(fade));
        c.text(note, type::monoLabel, {area.x, area.cy() + 4, area.w, 16}, label.mulAlpha(fade));
    };
    switch (state_) {
    case LyricsState::Loading: {
        // A placeholder only when the lyrics take a while (a cached file answers before it would flash).
        const double waited = ui::frame::realNow() - loadingSince_;
        if (waited < 350) {
            if (auto* w = window()) w->invalidateAfter(360 - waited);
            return;
        }
        const float a = smooth(static_cast<float>((waited - 350) / 300)) * tabIn_.value();
        c.pushOpacity(a);
        for (int i = 0; i < 5; ++i)
            ui::paintSkeleton(c, this, {area.x, area.y + 40 + i * 48.f, area.w * (0.4f + 0.1f * ((i * 7) % 4)), 22}, gfx::radii().badge);
        c.popLayer();
        if (a < 0.999f) ui::frame::requestNext();   // the fade-in; the shimmer paces itself (ui::paintSkeleton)
        return;
    }
    case LyricsState::Missing: {
        if (pick_.kind == lyrics::Choice::Kind::None)
            centered(tr(L"Bu şarkıda sözleri kapattın."), toUpperTr(tr(L"Senin seçimin")).c_str());
        else
            centered(tr(L"Bu şarkı için söz bulunamadı."), askedSpotify_ ? tr(L"KAYNAK · LRCLIB · SPOTIFY") : tr(L"KAYNAK · LRCLIB"));
        // Find them by hand (app/LyricsPicker); not when lyrics are off in Ayarlar.
        if (Settings::get().lyricsEnabled) {
            const std::wstring link = tr(L"Sözleri ara");
            auto ll = gfx::makeLayout(link, type::body, 400);
            DWRITE_TEXT_METRICS lm{};
            ll->GetMetrics(&lm);
            const float lw = std::ceil(lm.widthIncludingTrailingWhitespace) + 4;
            pickerRect_ = {area.x, area.cy() + 32, lw, 24};
            c.text(link, type::body, pickerRect_, (hoverPicker_ ? accent().base : col.fgPrimary).mulAlpha(fade), gfx::TextAlign::Leading,
                   gfx::VAlign::Center);
        }
        return;
    }
    case LyricsState::Instrumental: centered(tr(L"♪  Enstrümantal"), tr(L"SÖZ YOK · SADECE MÜZİK")); return;
    case LyricsState::None: return;
    default: break;
    }
    const int pos = lyricsClockMs(trackId_);   // on the song's timeline, offset applied
    const bool synced = state_ == LyricsState::Synced;
    const int active = synced ? lyrics::activeLine(lyrics_, pos + 150) : -1;   // 150 ms lookahead feels in sync
    if (active != active_) {
        active_ = active;
        activeText_.setText(active >= 0 ? lines_[active].text() : L"");
    }
    // Layout all lines (heights cached) and find the active line's top.
    const float gap = wide_ ? 18.f : 14.f;
    lineRects_.resize(lines_.size());
    float y = 0, activeTop = 0, activeH = 0;
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        const float hh = lineHeight(i, area.w) + translationHeight(i, area.w);   // a line's rect holds its translation
        lineRects_[i] = {area.x, y, area.w, hh};
        if (i == active_) {
            activeTop = y;
            activeH = hh;
        }
        y += hh + gap;
    }
    const float total = y;
    const float lo = -area.h * 0.5f, hi = std::max(0.f, total - area.h * 0.5f);
    scroll_.setBounds(lo, hi);
    if (ui::frame::realNow() >= manualUntil_) {   // the wheel's offset holds for 3 s, then the active line leads again
        float target = scroll_.target();
        if (synced && active_ >= 0) target = activeTop + activeH * 0.5f - area.h * 0.42f;
        else if (synced) target = -area.h * 0.3f;
        target = std::clamp(target, lo, hi);
        if (std::abs(target - scroll_.target()) > 0.5f) {
            // Freshly loaded lyrics start at the active line instead of scrolling there from the top.
            if (fade < 0.05f) scroll_.snap(target);
            else scroll_.glideTo(target, ui::ScrollFeel::Follow);
        }
    }
    const float off = scroll_.value();
    // Lines fade out towards the area's edges (a line at a time: no layer over the backdrop).
    const float edge = std::min(72.f, area.h * 0.18f);
    c.pushClip(area);
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        Rect lr = lineRects_[i].offset(0, area.y - off);
        lineRects_[i] = lr;
        const bool hasTrans = i < static_cast<int>(trans_.size()) && !trans_[i].empty();
        if (lr.bottom() < area.y || lr.y > area.bottom()) {
            lines_[i].reset();   // scrolled away: drop its layout (no-op when it has none)
            if (hasTrans) trans_[i].reset();
            continue;
        }
        Color colr;
        if (!synced) colr = col.fgSecondary;
        else if (i == active_) colr = col.fgPrimary;
        else {
            const int dist = std::abs(i - active_);
            const float a = i < active_ ? 0.38f : 0.26f;
            colr = col.fgPrimary.withAlpha(dist >= 3 ? a * 0.65f : a);
        }
        if (i == hoverLine_ && synced && i != active_) colr = col.fgPrimary.withAlpha(0.72f);
        const float edgeA = smooth((lr.cy() - area.y) / edge) * smooth((area.bottom() - lr.cy()) / edge);
        colr = colr.mulAlpha(fade * (0.15f + 0.85f * edgeA));
        const float th = translationHeight(i, area.w);
        const Rect main{lr.x, lr.y, lr.w, lr.h - th};
        if (i == active_ && synced) c.text(activeText_, main, colr);
        else c.text(lines_[i], main, colr);
        if (hasTrans) c.text(trans_[i], {lr.x, main.bottom() + kTranslationGap, lr.w, th - kTranslationGap}, colr.mulAlpha(0.62f));
    }
    c.popClip();
}

// Live radio in the lyrics' place: the songs the station announced this session with the time each one started (the
// current one on top; the big title on the left already names it). The station's details are in Hakkında.
void NowPlayingView::paintLive(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    const Color label = backdrop_.labelColor();
    const float fade = tabIn_.value();
    const auto* p = ctx().player;
    const Rect area{r.x, r.y + 24, r.w, std::max(0.f, r.h - 24)};
    const auto& heard = heardTitles();
    const std::wstring now = p ? p->liveTitle() : std::wstring();
    if (heard.empty()) {
        const bool connecting = p && (p->status() == player::Status::Resolving || p->status() == player::Status::Buffering);
        c.text(tr(L"Şu an yayında"), type::headline, {area.x, area.cy() - 40, area.w, 34}, col.fgSecondary.mulAlpha(fade));
        c.text(connecting ? toUpperTr(tr(L"Bağlanıyor…")) : toUpperTr(tr(L"Şarkı bilgisi gelince burada görünür")), type::monoLabel,
               {area.x, area.cy() + 4, area.w, 16}, label.mulAlpha(fade));
        return;
    }
    auto clock = [](int64_t at) {
        const std::time_t tt = static_cast<std::time_t>(at);
        std::tm tm{};
        localtime_s(&tm, &tt);
        wchar_t hhmm[8];
        swprintf(hhmm, 8, L"%02d:%02d", tm.tm_hour, tm.tm_min);
        return std::wstring(hhmm);
    };
    float y = area.y;
    for (size_t i = 0; i < heard.size(); ++i) {
        const bool current = i == 0 && !now.empty() && heard[0].title == now;
        if (current) {
            // The song on air: its start time and "ŞİMDİ" in the accent, the title in the section size.
            c.text(clock(heard[i].at) + L"  ·  " + tr(L"ŞİMDİ"), type::monoLabel, {area.x, y, area.w, 16}, acc.base.mulAlpha(fade));
            liveNow_.setText(heard[i].title);
            const float nh = liveNow_.measure(area.w).h;
            c.text(liveNow_, {area.x, y + 24, area.w, nh}, col.fgPrimary.mulAlpha(fade));
            y += 24 + nh + 36;
            if (heard.size() > 1) {
                c.text(toUpperTr(tr(L"Az önce çalanlar")), type::monoLabel, {area.x, y, area.w, 16}, label.mulAlpha(fade));
                y += 28;
            }
            continue;
        }
        if (y + 34 > area.bottom()) break;
        c.text(clock(heard[i].at), type::monoMeta, {area.x, y, 56, 24}, label.mulAlpha(fade), gfx::TextAlign::Leading, gfx::VAlign::Center);
        c.text(heard[i].title, type::bodyL, {area.x + 64, y, area.w - 64, 24}, col.fgSecondary.mulAlpha(fade), gfx::TextAlign::Leading,
               gfx::VAlign::Center);
        y += 34;
    }
    if (heard.size() == 1 && y + 16 < area.bottom())
        c.text(toUpperTr(tr(L"Bu istasyonda dinlediğin önceki şarkılar burada listelenir")), type::monoLabel, {area.x, y, area.w, 16},
               label.mulAlpha(fade));
}

int NowPlayingView::lineAt(gfx::Point p) const {
    for (int i = 0; i < static_cast<int>(lineRects_.size()); ++i)
        if (lineRects_[i].contains(p)) return i;
    return -1;
}

// ---- Hakkında --------------------------------------------------------------------------------------------------------

void NowPlayingView::fetchAbout() {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t || kind() != Kind::Song) return;
    if (about_.trackId == t->id && about_.state != About::State::None) return;
    about_ = {};
    about_.trackId = t->id;
    fetchCredits();   // any song: MusicBrainz also finds local files and previews by name
    const std::string albumId = catalogId(t->album.id) ? t->album.id : std::string();
    const std::string artistId = !t->artists.empty() && catalogId(t->artists[0].id) ? t->artists[0].id : std::string();
    if (albumId.empty() && artistId.empty()) {   // a local file, a preview: only its own fields
        about_.state = About::State::Ready;
        return;
    }
    about_.state = About::State::Loading;
    spotify::Api* api = source::activeApi();
    async(
        Priority::Normal, life_.ref(),
        [api, albumId, artistId] {
            AboutFetch f;
            int asked = 0, failed = 0;
            if (!albumId.empty()) {
                ++asked;
                try {
                    auto a = source::album(api, albumId);
                    if (a.totalTracks <= 0) a.totalTracks = static_cast<int>(a.tracks.size());
                    a.tracks.clear();
                    a.tracks.shrink_to_fit();
                    f.album = std::move(a);
                } catch (const std::exception& e) {
                    ++failed;
                    ST_LOG_DEBUG("nowplaying", "album details: {}", e.what());
                }
            }
            if (!artistId.empty()) {
                ++asked;
                try {
                    auto page = source::artistPage(api, artistId);
                    f.artist = std::move(page.artist);
                    f.similar = std::move(page.similar);
                    if (f.similar.size() > 8) f.similar.resize(8);
                } catch (const std::exception& e) {
                    ++failed;
                    ST_LOG_DEBUG("nowplaying", "artist details: {}", e.what());
                }
            }
            f.failed = asked > 0 && failed == asked;
            return f;
        },
        [this](Result<AboutFetch> r) {
            if (!r || r->failed) {
                about_.state = About::State::Failed;
            } else {
                about_.album = std::move(r->album);
                about_.artist = std::move(r->artist);
                about_.similar = std::move(r->similar);
                about_.state = About::State::Ready;
            }
            aboutIn_.snap(0.75f);   // the details join what the track itself said: a gentle refresh
            aboutIn_.to(1, ui::motion::slow, ui::Ease::Decelerate);
            invalidate();
        });
}

// Song credits (app/Credits): asked once per track while Hakkında is open, like the details; the cache answers at once.
void NowPlayingView::fetchCredits() {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t || kind() != Kind::Song) return;
    about_.creditsState = About::State::Loading;
    creditTexts_.clear();
    const std::string id = t->id;
    async(Priority::Normal, life_.ref(),
          [req = credits::requestFor(source::activeApi(), *t)] { return credits::fetch(req); },
          [this, id](Result<std::optional<catalog::Credits>> r) {
              if (about_.trackId != id) return;
              about_.credits = r ? *r : std::optional<catalog::Credits>{};
              about_.creditsState = about_.credits ? About::State::Ready : About::State::Failed;
              about_.summary = about_.credits ? credits::summary(*about_.credits) : std::vector<catalog::Credit>{};
              creditTexts_.clear();
              for (const auto& p : about_.summary)
                  creditTexts_.emplace_back(gfx::Text(toWide(p.name), type::body), gfx::Text(creditRolesText(p.roles), type::caption));
              invalidate();
          });
}

// Hakkında's credits: the first people (performers, writers, producers; each artist a link), "Tüm krediler" (the
// dialog) and the sources.
float NowPlayingView::paintAboutCredits(Canvas& c, float x, float y, float w) {
    const auto state = about_.creditsState;
    if (state == About::State::None) return 0;
    const auto& col = colors();
    const Color label = backdrop_.labelColor();
    const float fade = aboutIn_.value() * tabIn_.value();
    const float y0 = y;
    c.text(toUpperTr(tr(L"Krediler")), type::monoLabel, {x, y, w, 14}, label.mulAlpha(fade));
    y += 24;
    if (state == About::State::Loading) {
        for (int i = 0; i < 3; ++i) {
            ui::paintSkeleton(c, this, {x, y + 4, w * (0.42f - 0.07f * static_cast<float>(i)), 12}, gfx::radii().badge);
            ui::paintSkeleton(c, this, {x, y + 24, w * 0.26f, 9}, gfx::radii().badge);
            y += 46;
        }
        return y - y0 + 22;
    }
    const catalog::Credits* cr = about_.credits ? &*about_.credits : nullptr;
    if (!cr || cr->empty()) {
        c.text(!cr ? tr(L"Krediler yüklenemedi. Bağlantını kontrol et.") : tr(L"Bu şarkı için kredi bilgisi yok."), type::caption,
               {x, y, w, 18}, col.fgSecondary.mulAlpha(fade), gfx::TextAlign::Leading, gfx::VAlign::Center);
        y += 26;
    } else {
        for (size_t i = 0; i < about_.summary.size() && i < creditTexts_.size(); ++i) {
            const auto& p = about_.summary[i];
            auto& [name, roles] = creditTexts_[i];
            const bool linked = !p.artistId.empty();
            const bool hot = linked && static_cast<int>(links_.size()) == hoverLink_;
            const float nw = std::min(w, std::ceil(name.measure().w) + 1);
            c.text(name, {x, y, nw, 20}, (hot ? accent().base : col.fgPrimary).mulAlpha(fade), gfx::VAlign::Center);
            if (linked) links_.push_back({{x, y, nw, 20}, [id = p.artistId] { ctx().router->navigate({RouteKind::Artist, id}); }});
            if (!roles.empty()) c.text(roles, {x, y + 20, w, 18}, col.fgSecondary.mulAlpha(fade), gfx::VAlign::Center);
            y += 46;
        }
        const catalog::Track track = *ctx().player->current();
        paintLink(c, tr(L"Tüm krediler"), x, y, [track] { showCredits(track); });
        y += 28;
    }
    if (cr && !cr->sources.empty()) {
        std::wstring names;
        for (const auto& s : cr->sources) names += (names.empty() ? L"" : L" · ") + toWide(s);
        // "KAYNAK" in the labels' mono, the names as they are written (no upper-casing of brand names).
        const std::wstring head = toUpperTr(tr(L"Kaynak"));
        auto hl = gfx::makeLayout(head, type::monoLabel, 400);
        DWRITE_TEXT_METRICS hm{};
        hl->GetMetrics(&hm);
        const float hw = std::ceil(hm.widthIncludingTrailingWhitespace) + 10;
        c.text(head, type::monoLabel, {x, y, hw, 18}, label.mulAlpha(fade), gfx::TextAlign::Leading, gfx::VAlign::Center);
        c.text(names, type::caption, {x + hw, y, std::max(40.f, w - hw), 18}, col.fgSecondary.mulAlpha(fade),
               gfx::TextAlign::Leading, gfx::VAlign::Center);
        y += 24;
    }
    return y - y0 + 8;
}

float NowPlayingView::paintLink(Canvas& c, const std::wstring& text, float x, float y, std::function<void()> go) {
    auto it = std::find_if(linkTexts_.begin(), linkTexts_.end(), [&](const auto& kv) { return kv.first == text; });
    if (it == linkTexts_.end()) {
        linkTexts_.emplace_back(text, gfx::Text(text, type::caption.withWeight(600)));
        it = std::prev(linkTexts_.end());
    }
    const float w = std::ceil(it->second.measure().w);
    const Rect r{x, y, w + 18, 20};
    const bool hot = static_cast<int>(links_.size()) == hoverLink_;
    const Color fg = hot ? accent().base : colors().fgPrimary;
    c.text(it->second, {x, y, w + 1, 20}, fg.mulAlpha(aboutIn_.value() * tabIn_.value()), gfx::VAlign::Center);
    c.icon("arrow-up-right", {x + w + 4, y + 3, 14, 14}, fg.mulAlpha(aboutIn_.value() * tabIn_.value()));
    links_.push_back({r, std::move(go)});
    return w + 18;
}

float NowPlayingView::paintAboutSong(Canvas& c, float x, float y, float w) {
    const auto& col = colors();
    const Color label = backdrop_.labelColor();
    const float fade = aboutIn_.value() * tabIn_.value();
    const auto* t = ctx().player->current();
    const float y0 = y;
    const bool loading = about_.state == About::State::Loading;
    // The album: its picture, name, kind and year, length and label.
    if (!t->album.name.empty() || about_.album) {
        const catalog::Album* a = about_.album ? &*about_.album : nullptr;
        std::wstring kindLabel = tr(L"Albüm");
        if (a && a->primaryType == "Single") kindLabel = tr(L"Tekli");
        else if (a && a->primaryType == "EP") kindLabel = L"EP";
        std::wstring lab = toUpperTr(kindLabel);
        if (a && !a->year().empty()) lab += L" · " + toWide(a->year());
        c.text(lab, type::monoLabel, {x, y, w, 14}, label.mulAlpha(fade));
        y += 24;
        const Rect art{x, y, kAboutArt, kAboutArt};
        drawArtwork(c, a && !a->images.empty() ? a->images : t->album.images, art, gfx::radii().art);
        c.strokeRounded(art, gfx::radii().art, col.hairDefault);
        const float tx = art.right() + 18, tw = std::max(60.f, w - kAboutArt - 18);
        aboutTitle_.setText(a ? toWide(a->name) : toWide(t->album.name));
        const float th = std::ceil(aboutTitle_.measure(tw).h);
        c.text(aboutTitle_, {tx, y, tw, th}, col.fgPrimary.mulAlpha(fade));
        float ty = y + th + 4;
        if (a) {
            std::wstring meta;
            if (a->totalTracks > 0) meta = i18n::plural(L"{} şarkı", a->totalTracks);
            if (!a->label.empty()) meta += (meta.empty() ? L"" : L" · ") + toWide(a->label);
            if (!meta.empty()) {
                c.text(meta, type::caption, {tx, ty, tw, 18}, col.fgSecondary.mulAlpha(fade));
                ty += 22;
            }
        } else if (loading) {
            ui::paintSkeleton(c, this, {tx, ty + 4, tw * 0.45f, 10}, gfx::radii().badge);
            ty += 22;
        }
        if (!t->album.id.empty()) {
            const std::string id = t->album.id;
            paintLink(c, tr(L"Albüme git"), tx, ty + 2, [id] { ctx().router->navigate({RouteKind::Album, id}); });
            ty += 24;
        }
        y = std::max(art.bottom(), ty) + 32;
    }
    // The (first) artist: photo, name, kind / country / start year / genre, listens.
    if (!t->artists.empty() && !t->artists[0].name.empty()) {
        const catalog::Artist* a = about_.artist ? &*about_.artist : nullptr;
        std::wstring lab = a && a->type == "Group" ? toUpperTr(tr(L"Grup")) : toUpperTr(tr(L"Sanatçı"));
        if (a && !a->country.empty()) lab += L" · " + toWide(a->country);
        if (a && !a->beginYear.empty()) lab += L" · " + toWide(a->beginYear);
        if (a && !a->tags.empty()) lab += L" · " + toUpperData(toWide(a->tags[0]));
        c.text(lab, type::monoLabel, {x, y, w, 14}, label.mulAlpha(fade));
        y += 24;
        const Rect art{x, y, kAboutArt, kAboutArt};
        drawArtwork(c, a ? a->images : std::vector<catalog::Image>{}, art, 0, Placeholder::Artist, true);
        c.strokeCircle({art.cx(), art.cy()}, kAboutArt * 0.5f, col.hairDefault);
        const float tx = art.right() + 18, tw = std::max(60.f, w - kAboutArt - 18);
        aboutName_.setText(a && !a->name.empty() ? toWide(a->name) : toWide(t->artists[0].name));
        const float th = std::ceil(aboutName_.measure(tw).h);
        c.text(aboutName_, {tx, y, tw, th}, col.fgPrimary.mulAlpha(fade));
        float ty = y + th + 4;
        std::wstring meta;
        if (a && a->listenCount > 0)
            meta = i18n::plural(L"{} DİNLENME · LISTENBRAINZ", a->listenCount, {compactCount(a->listenCount)});
        else if (a && !a->disambiguation.empty())
            meta = toUpperData(toWide(a->disambiguation));
        if (!meta.empty()) {
            c.text(meta, type::monoMeta, {tx, ty, tw, 18}, col.fgSecondary.mulAlpha(fade));
            ty += 22;
        } else if (loading && !a) {
            ui::paintSkeleton(c, this, {tx, ty + 4, tw * 0.35f, 10}, gfx::radii().badge);
            ty += 22;
        }
        if (!t->artists[0].id.empty()) {
            const std::string id = t->artists[0].id;
            paintLink(c, tr(L"Sanatçıya git"), tx, ty + 2, [id] { ctx().router->navigate({RouteKind::Artist, id}); });
            ty += 24;
        }
        y = std::max(art.bottom(), ty) + 32;
    }
    // Who made it.
    if (const float h = paintAboutCredits(c, x, y, w); h > 0) y += h + 24;
    // Similar artists: outlined pills, each to its page.
    if (!about_.similar.empty()) {
        c.text(toUpperTr(tr(L"Benzer sanatçılar")), type::monoLabel, {x, y, w, 14}, label.mulAlpha(fade));
        y += 24;
        if (chipTexts_.size() != about_.similar.size()) {
            chipTexts_.clear();
            for (const auto& s : about_.similar) chipTexts_.emplace_back(toWide(s.name), type::caption.withWeight(600));
        }
        float cx = x;
        for (size_t i = 0; i < about_.similar.size(); ++i) {
            const float cw = std::min(w, std::ceil(chipTexts_[i].measure().w) + 24);
            if (cx > x && cx + cw > x + w) {
                cx = x;
                y += 30 + 8;
            }
            const Rect chip{cx, y, cw, 30};
            const bool hot = static_cast<int>(links_.size()) == hoverLink_;
            if (hot) c.fillPill(chip, col.overlayHover.mulAlpha(fade));
            c.strokePill(chip, (hot ? col.fgSecondary : col.hairControl).mulAlpha(fade));
            c.text(chipTexts_[i], chip.inset(12, 0), (hot ? col.fgPrimary : col.fgSecondary).mulAlpha(fade), gfx::VAlign::Center);
            const std::string id = about_.similar[i].id;
            links_.push_back({chip, [id] {
                                  if (!id.empty()) ctx().router->navigate({RouteKind::Artist, id});
                              }});
            cx += cw + 8;
        }
        y += 30 + 32;
    }
    if (y == y0) {
        c.text(tr(L"Bu şarkı için ek bilgi yok."), type::bodyL, {x, y, w, 24}, col.fgSecondary.mulAlpha(fade));
        y += 32;
    }
    if (about_.state == About::State::Failed) {
        c.text(toUpperTr(tr(L"Bilgiler yüklenemedi.")), type::monoLabel, {x, y, w, 14}, label.mulAlpha(fade));
        y += 24;
    }
    return y - y0;
}

float NowPlayingView::paintAboutEpisode(Canvas& c, float x, float y, float w, float h) {
    const auto& col = colors();
    const Color label = backdrop_.labelColor();
    const float fade = tabIn_.value();
    const auto* t = ctx().player->current();
    const float y0 = y;
    // The show: picture, name, the episode's date and length, its page.
    const Rect art{x, y, kAboutArt, kAboutArt};
    drawArtwork(c, t->album.images, art, gfx::radii().art);
    c.strokeRounded(art, gfx::radii().art, col.hairDefault);
    const float tx = art.right() + 18, tw = std::max(60.f, w - kAboutArt - 18);
    aboutTitle_.setText(toWide(t->album.name.empty() ? t->artistLine() : t->album.name));
    const float th = std::ceil(aboutTitle_.measure(tw).h);
    c.text(aboutTitle_, {tx, y, tw, th}, col.fgPrimary.mulAlpha(fade));
    float ty = y + th + 4;
    std::wstring meta = ui::formatDuration(ctx().player->durationMs());
    if (t->addedAt > 0) meta = trDate(t->addedAt, true) + L" · " + meta;
    c.text(meta, type::monoMeta, {tx, ty, tw, 18}, label.mulAlpha(fade));
    ty += 22;
    const catalog::Track episode = *t;
    paintLink(c, tr(L"Podcast'e git"), tx, ty + 2, [episode] { openEpisodeShow(episode); });
    ty += 24;
    y = std::max(art.bottom(), ty) + 28;
    // The notes take the rest (whole lines; long ones end with "…").
    if (h - (y - y0) > 40) paintEpisodeNotes(c, {x, y, w, h - (y - y0)}, *t);
    return h;
}

float NowPlayingView::paintAboutStation(Canvas& c, float x, float y, float w) {
    const auto& col = colors();
    const Color label = backdrop_.labelColor();
    const float fade = tabIn_.value();
    const auto* t = ctx().player->current();
    const float y0 = y;
    const Rect art{x, y, kAboutArt, kAboutArt};
    c.fillRounded(art, gfx::radii().art, gfx::isLightTheme() ? col.bgSunken : col.bgOverlay);
    drawStationArt(c, t->album.images, art.center(std::round(kAboutArt * 0.6f), std::round(kAboutArt * 0.6f)), gfx::radii().art);
    c.strokeRounded(art, gfx::radii().art, col.hairDefault);
    const float tx = art.right() + 18, tw = std::max(60.f, w - kAboutArt - 18);
    aboutTitle_.setText(toWide(t->name));
    const float th = std::ceil(aboutTitle_.measure(tw).h);
    c.text(aboutTitle_, {tx, y, tw, th}, col.fgPrimary.mulAlpha(fade));
    float ty = y + th + 4;
    // "Country · genres" and the stream's codec / bitrate.
    c.text(toWide(t->artistLine()), type::caption, {tx, ty, tw, 18}, col.fgSecondary.mulAlpha(fade));
    ty += 22;
    const auto* s = radio::store().find(radio::uuidOf(t->id));
    if (s) {
        c.text(toWide(radio::codecBadge(*s)), type::monoMeta, {tx, ty, tw, 18}, label.mulAlpha(fade));
        ty += 22;
    }
    if (s && !s->homepage.empty()) {
        std::wstring host = toWide(s->homepage);
        if (const size_t scheme = host.find(L"://"); scheme != std::wstring::npos) host = host.substr(scheme + 3);
        if (const size_t slash = host.find(L'/'); slash != std::wstring::npos) host.resize(slash);
        if (host.rfind(L"www.", 0) == 0) host = host.substr(4);
        const std::wstring url = toWide(s->homepage);
        paintLink(c, host, tx, ty + 2, [url] { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        ty += 24;
    }
    paintLink(c, tr(L"Radyo"), tx, ty + 2, [] { ctx().router->navigate({RouteKind::Radio}); });
    ty += 24;
    y = std::max(art.bottom(), ty) + 28;
    return y - y0;
}

void NowPlayingView::paintAbout(Canvas& c, const Rect& r) {
    links_.clear();
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t) return;
    const Rect area{r.x, r.y + 16, r.w, std::max(0.f, r.h - 16)};
    const Kind k = kind();
    if (k == Kind::Episode) {   // fills the area: the notes are clipped to it
        paintAboutEpisode(c, area.x, area.y, std::min(area.w, 640.f), area.h);
        return;
    }
    aboutScroll_.setBounds(0, std::max(0.f, aboutH_ - area.h));
    const float off = aboutScroll_.value();
    c.pushClip(area);
    const float w = std::min(area.w, 560.f);
    aboutH_ = k == Kind::Station ? paintAboutStation(c, area.x, area.y - off, w) : paintAboutSong(c, area.x, area.y - off, w);
    c.popClip();
    // Links scrolled out of the area are not clickable.
    for (auto& l : links_)
        if (!area.contains({l.r.cx(), l.r.cy()})) l.r = {};
}

// ---- painting ----------------------------------------------------------------------------------------------------------

// nowPlayingArtShadow (blur 60, offset y 30, shadow.art) without the Shadow effect. The Gaussian blur of the art's
// rectangle is separable - alpha(x, y) = gx(x) * gy(y), each an erf step - so it is computed once on the CPU into a
// small bitmap (1 px per 4 DIP: after a 20 DIP std-dev blur only low frequencies are left) and stretched with linear
// filtering (~100 KB). An effect would re-run every frame for a result that only changes with the art size / theme.
void NowPlayingView::paintArtShadow(Canvas& c, const Rect& art) {
    constexpr float kSigma = 60.f / 3.f, kMargin = 3 * kSigma, kOffsetY = 26.f, kDipPerPx = 4.f;
    const Color color = colors().shadowArt.mulAlpha(0.85f);   // a touch softer over the coloured backdrop
    const Rect dst{art.x - kMargin, art.y + kOffsetY - kMargin, art.w + 2 * kMargin, art.h + 2 * kMargin};
    const uint64_t gen = gfx::Device::get().generation(), themeGen = gfx::Theme::get().generation();
    if (!shadow_ || shadowArt_ != art.w || shadowGen_ != gen || shadowTheme_ != themeGen) {
        shadow_.Reset();
        const UINT w = static_cast<UINT>(std::ceil(dst.w / kDipPerPx)), h = static_cast<UINT>(std::ceil(dst.h / kDipPerPx));
        // Coverage of [lo, hi] blurred, sampled at pixel centers (DIP offsets from dst's origin).
        auto profile = [](UINT n, float len, float lo, float hi) {
            std::vector<float> v(n);
            const float k = 1.f / (kSigma * std::numbers::sqrt2_v<float>);
            for (UINT i = 0; i < n; ++i) {
                const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
                v[i] = 0.5f * (std::erf((x - lo) * k) - std::erf((x - hi) * k));
            }
            return v;
        };
        const auto gx = profile(w, dst.w, kMargin, kMargin + art.w);
        const auto gy = profile(h, dst.h, kMargin, kMargin + art.h);
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        auto channel = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const float a = color.a * gx[x] * gy[y];   // premultiplied BGRA, like the effect's output
                px[static_cast<size_t>(y) * w + x] =
                    channel(color.b * a) | channel(color.g * a) << 8 | channel(color.r * a) << 16 | channel(a) << 24;
            }
        }
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(c.dc()->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &props, &shadow_))) return;
        shadowArt_ = art.w;
        shadowGen_ = gen;
        shadowTheme_ = themeGen;
    }
    c.dc()->DrawBitmap(shadow_.Get(), dst.d2d(), 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
}

void NowPlayingView::paintCover(Canvas& c, const Rect& r, bool outgoing) {
    const auto& col = colors();
    const Shown& s = outgoing ? old_ : shown_;
    // Video mode's picture is on screen in the cover's place (video_ paints it with the children): no cover, shadow or
    // outline under it, and none sliding with a track change either.
    if (video_->hidesHostArt()) return;
    paintArtShadow(c, r);
    if (s.kind == Kind::Station) {
        // A station logo is a small favicon: shown at under half the size on the surface with the tiles' ring
        // motif instead of blown up to the hero size.
        c.fillRounded(r, gfx::radii().art, gfx::isLightTheme() ? col.bgSunken : col.bgOverlay);
        c.strokeCircle({r.cx(), r.cy()}, r.w * 0.36f, accent().base.withAlpha(0.35f), 1.f);
        const float d = std::round(r.w * 0.42f);
        drawStationArt(c, s.images, r.center(d, d), gfx::radii().art);
    } else if (outgoing) {
        drawArtwork(c, s.images, r, gfx::radii().art);
    } else {
        art_.draw(c, s.images, r, gfx::radii().art);   // a cover still loading fades in over the placeholder
    }
    c.strokeRounded(r, gfx::radii().art, col.hairDefault);   // the thin outline
}

void NowPlayingView::paintTrack(Canvas& c) {
    const auto& col = colors();
    const auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    if (!t) return;
    const Color label = backdrop_.labelColor();
    // The cover and the titles change like the player bar's: the old ones slide out and fade while the new ones slide
    // in (layers only for those 320 ms, clipped to the track block).
    const Rect block{std::min(artRect_.x, infoRect_.x) - 140, artRect_.y - 70, 0, 0};
    const Rect clip{block.x, block.y, std::max(artRect_.right(), infoRect_.right()) + 140 - block.x,
                    std::max(subRect_.bottom(), artRect_.bottom()) + 100 - block.y};
    if (swap_.running()) {
        const auto out = swap_.outgoing();
        if (out.alpha > 0.001f) {
            c.pushClip(clip);
            c.pushOpacity(out.alpha);
            c.pushTransform(D2D1::Matrix3x2F::Translation(0, out.dy * kSwapShift));
            paintCover(c, artRect_, true);
            c.text(oldTitle_, titleRect_, col.fgPrimary);
            c.text(oldSub_, subRect_, col.fgSecondary, gfx::VAlign::Center);
            c.popTransform();
            c.popLayer();
            c.popClip();
        }
    }
    const auto in = swap_.incoming();
    const bool group = in.alpha < 0.999f;
    if (group) {
        c.pushClip(clip);
        c.pushOpacity(in.alpha);
    }
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, in.dy * kSwapShift));
    paintCover(c, artRect_, false);
    c.text(title_, titleRect_, col.fgPrimary);
    if (hoverTitle_) {   // a link: underlined while the pointer is on it
        if (auto* l = title_.layout(titleRect_.w)) {
            DWRITE_LINE_METRICS lm[4];
            UINT32 n = 0;
            l->GetLineMetrics(lm, 4, &n);
            float ly = titleRect_.y;
            UINT32 pos = 0;
            for (UINT32 i = 0; i < n && i < 4; ++i) {
                DWRITE_HIT_TEST_METRICS hm[4];
                UINT32 hn = 0;
                l->HitTestTextRange(pos, lm[i].length - lm[i].trailingWhitespaceLength, titleRect_.x, titleRect_.y, hm, 4, &hn);
                for (UINT32 k = 0; k < hn; ++k) c.hline(hm[k].left, hm[k].left + hm[k].width, ly + lm[i].baseline + 4, col.fgPrimary.withAlpha(0.6f));
                ly += lm[i].height;
                pos += lm[i].length;
            }
        }
    }
    // The artist line: each artist (and the album) a link of its own.
    c.text(sub_, subRect_, col.fgSecondary, gfx::VAlign::Center);
    subLinkRects_.assign(subLinks_.size(), Rect{});
    if (!subLinks_.empty()) {
        if (auto* l = sub_.layout(subRect_.w)) {
            DWRITE_TEXT_METRICS tm{};
            l->GetMetrics(&tm);
            const float oy = subRect_.y + (subRect_.h - tm.height) * 0.5f;
            for (size_t i = 0; i < subLinks_.size(); ++i) {
                DWRITE_HIT_TEST_METRICS hm[2];
                UINT32 hn = 0;
                if (FAILED(l->HitTestTextRange(subLinks_[i].from, subLinks_[i].len, subRect_.x, oy, hm, 2, &hn)) || hn == 0) continue;
                const Rect lr{hm[0].left, hm[0].top, hm[0].width, hm[0].height};
                if (lr.w <= 0.5f) continue;
                subLinkRects_[i] = lr;
                if (static_cast<int>(i) == hoverSub_) c.hline(lr.x, lr.right(), lr.bottom() - 1, col.fgSecondary);
            }
        }
    }
    c.popTransform();
    if (group) {
        c.popLayer();
        c.popClip();
    }

    // The match line (changes in place).
    const float y = metaRect_.y;
    if (radio::isStationId(t->id)) {
        // "CANLI" + the stream's codec / bitrate; nothing was matched, so no "Yanlış eşleşme?".
        const float bw = drawLiveBadge(c, {metaRect_.x, metaRect_.cy()}, p->isPlaying());
        if (const auto* s = radio::store().find(radio::uuidOf(t->id)))
            c.text(toWide(radio::codecBadge(*s)), type::monoMeta, {metaRect_.x + bw + 12, y, metaRect_.w - bw - 12, metaRect_.h}, label,
                   gfx::TextAlign::Leading, gfx::VAlign::Center);
        matchRect_ = {};
    } else if (catalog::isPodcastId(t->id)) {
        meta_.setText(ui::formatDuration(p->durationMs()));   // an episode plays from its feed: its length
        c.text(meta_, metaRect_, label, gfx::VAlign::Center);
        matchRect_ = {};
    } else {
        // Like Spotube, no stream / source wording: the duration and how sure the match is, next to "Yanlış eşleşme?".
        // A smart shuffle / "Geliştir" recommendation says so first.
        const float mx = t->recommended ? metaRect_.x + smartshuffle::drawBadge(c, metaRect_.x, metaRect_.cy()) + 10 : metaRect_.x;
        std::wstring meta = ui::formatDuration(p->durationMs());
        if (auto m = p->currentMatch()) {
            const int pct = static_cast<int>(std::round(std::clamp(m->score, 0.0, 100.0)));
            meta = i18n::format(tr(L"{} · EŞLEŞME %{}"), {meta, std::to_wstring(pct)});   // "03:42 · EŞLEŞME %87"
        }
        meta_.setText(meta);
        const float mw = std::ceil(meta_.measure().w);
        c.text(meta_, {mx, y, mw + 1, metaRect_.h}, label, gfx::VAlign::Center);
        matchRect_ = {};
        if (t->id.rfind("local:", 0) != 0) {   // the user's own file: nothing was matched
            const std::wstring wrongMatch = tr(L"Yanlış eşleşme?");
            auto wl = gfx::makeLayout(wrongMatch, type::caption, 400);
            DWRITE_TEXT_METRICS wm{};
            wl->GetMetrics(&wm);
            matchRect_ = {mx + mw + 14, y - 1, std::ceil(wm.widthIncludingTrailingWhitespace) + 4, metaRect_.h + 2};
            c.text(wrongMatch, type::caption, matchRect_, hoverMatch_ ? accent().base : col.fgSecondary, gfx::TextAlign::Leading,
                   gfx::VAlign::Center);
        }
    }
    matchFocus_->place(matchRect_);
    // Times beside the wave: elapsed (fg.secondary) and total; a live stream has none (the "CANLI" badge says it).
    if (!radio::isStationId(t->id)) {
        const int64_t dur = p->durationMs();
        elapsed_.setText(ui::formatDuration(seek_->dragging() ? static_cast<int64_t>(seek_->value() * dur) : p->positionMs()));
        total_.setText(ui::formatDuration(dur));
        c.text(elapsed_, elapsedRect_, col.fgSecondary, gfx::VAlign::Center);
        c.text(total_, totalRect_, label, gfx::VAlign::Center);
    }
}

void NowPlayingView::paint(Canvas& c) {
    // Opening the view is a burst of first-time GPU work (display-size glyphs, new shader variants) whose scratch stays
    // in Direct2D's / the driver's caches next to what the pages behind it cached. Once the view has settled, hand that
    // back like the startup trim does (App::housekeeping). After the frame: the device must not be trimmed mid-draw.
    if (!trimmed_ && ui::frame::realNow() - openedAt_ > 5000) {
        trimmed_ = true;
        Dispatcher::post([] { gfx::Device::get().trim(); });
    }
    const Rect r = rect();
    const auto& col = colors();
    auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    const bool playing = p && p->isPlaying();
    // The full-screen lyrics cover this view: nothing here moves meanwhile (they keep their own rate).
    const bool covered = LyricsFullscreen::isOpen();
    const bool live = t && radio::isStationId(t->id);
    const int64_t dur = p ? p->durationMs() : 0;
    if (!seek_->dragging()) seek_->setValue(!live && dur > 0 ? static_cast<float>(p->positionMs()) / static_cast<float>(dur) : 0.f);
    seek_->setBuffered(p && !live ? p->bufferedFraction() : 0.f);
    if (swap_.running()) requestLayout();

    c.pushClip(r);
    const double driftMs = backdrop_.paint(c, r, t, playing && !covered);
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    paintTrack(c);
    // The tab column: a hairline under the tab row, then the open tab's content.
    c.hline(columnRect_.x, columnRect_.right(), columnRect_.y + kTabRowH - 0.5f, col.hairSubtle);
    switch (shownTab_) {
    case Tab::Lyrics:
        if (live) paintLive(c, contentRect_);
        else paintLyrics(c, contentRect_);
        break;
    case Tab::About: paintAbout(c, contentRect_); break;
    case Tab::Queue: break;   // the queue panel (a child)
    }
    c.popTransform();
    // Children; the queue panel fades in with its tab.
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    for (auto& child : children()) {
        if (!child->visible()) continue;
        if (child.get() == queue_ && tabIn_.value() < 0.999f) {
            c.pushOpacity(tabIn_.value());
            child->paint(c);
            c.popLayer();
            continue;
        }
        child->paint(c);
    }
    c.popTransform();
    // Hover time just above the wave.
    if (const float hf = seek_->hoverFraction(); hf >= 0 && dur > 0 && !live) {
        const Rect sr = seek_->rect();
        const std::wstring s = ui::formatDuration(static_cast<int64_t>(hf * dur));
        const Rect tip{r.x + sr.x + hf * sr.w - 24, r.y + sr.cy() - 6 - 22, 48, 18};
        c.fillRounded(tip, gfx::radii().tooltip, col.bgElevated);
        c.strokeRounded(tip, gfx::radii().tooltip, col.hairStrong);
        c.text(s, type::monoMeta, tip, col.fgPrimary, gfx::TextAlign::Center, gfx::VAlign::Center);
    }
    c.popClip();

    // Frames while playing: the wave flows at ~30 fps, synced lyrics follow at 20 fps, the backdrop drifts at 12 fps;
    // otherwise the time label ticks 4 times a second. Paused: no wake-ups at all.
    if (playing && !covered) {
        double next = 250;
        if (seek_->waveMoving()) next = ui::kWaveFrameMs;
        if (shownTab_ == Tab::Lyrics && state_ == LyricsState::Synced) next = std::min(next, 50.0);
        if (driftMs > 0) next = std::min(next, driftMs);
        if (auto* w = window()) {   // the flowing wave alone: paced by presentation (~30 fps at any refresh rate)
            if (next == ui::kWaveFrameMs) w->invalidateAfterShown(next);
            else w->invalidateAfter(next);
        }
    }
    if (devLog()) {
        ++framesSinceStats_;
        const double now = ui::frame::realNow();
        if (now - statsAt_ >= 5000) {
            const auto s = backdrop_.takeStats();
            const double secs = (now - statsAt_) / 1000.0;
            ST_LOG_INFO("nowplaying", "{:.1f} frames/s, backdrop {} x {} texels: {:.1f} renders/s, {:.3f} ms each", framesSinceStats_ / secs,
                        s.texelsW, s.texelsH, s.renders / secs, s.renders ? s.renderMs / s.renders : 0.0);
            statsAt_ = now;
            framesSinceStats_ = 0;
        }
    }
}

// ---- input -----------------------------------------------------------------------------------------------------------

void NowPlayingView::openTitle() {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t) return;
    if (radio::isStationId(t->id)) ctx().router->navigate({RouteKind::Radio});
    else if (catalog::isPodcastId(t->id)) openEpisodeShow(*t);
    else if (!t->album.id.empty()) ctx().router->navigate({RouteKind::Album, t->album.id});
}

void NowPlayingView::leaveTo(const std::function<void()>& go) {
    // The router's page change closes the view (App); called through a copy: it may rebuild what holds it.
    const auto run = go;
    if (run) run();
}

bool NowPlayingView::onWheel(float dy, float, const ui::MouseEvent& e) {
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    if (dy == 0 || !contentRect_.contains(lp)) return false;
    if (shownTab_ == Tab::Lyrics && !lines_.empty()) {
        scroll_.wheel(dy, 90.f, contentRect_.h * 0.8f);   // glides on from wherever the follow motion is
        manualUntil_ = ui::frame::realNow() + 3000;      // back to the active line after 3 s
        invalidate();
        return true;
    }
    if (shownTab_ == Tab::About && kind() != Kind::Episode) {
        aboutScroll_.wheel(dy, 90.f, contentRect_.h * 0.8f);
        invalidate();
        return true;
    }
    return false;
}

bool NowPlayingView::onMouseDown(const ui::MouseEvent& e) {
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t) return false;
    const bool onTrack = artRect_.contains(lp) || titleRect_.contains(lp) || subRect_.contains(lp);
    if (e.button == ui::MouseButton::Right) {
        if (!onTrack) return false;
        if (radio::isStationId(t->id)) showStationMenu(*t, e.windowPos);
        else showTrackMenu({*t}, e.windowPos);   // an episode goes on to its own menu
        return true;
    }
    if (e.button != ui::MouseButton::Left) return false;
    for (size_t i = 0; i < subLinkRects_.size() && i < subLinks_.size(); ++i) {
        if (subLinkRects_[i].contains(lp)) {
            leaveTo(subLinks_[i].go);
            return true;
        }
    }
    if (titleRect_.contains(lp)) {
        openTitle();
        return true;
    }
    if (matchRect_.contains(lp)) {
        if (ctx().showMatchPicker) ctx().showMatchPicker(*t);
        return true;
    }
    if (shownTab_ == Tab::About) {
        for (const auto& l : links_) {
            if (l.r.contains(lp)) {
                leaveTo(l.go);
                return true;
            }
        }
    }
    if (shownTab_ == Tab::Lyrics && kind() == Kind::Song && pickerRect_.contains(lp)) {
        showLyricsPicker(*t, lyrics_.lrclibId);   // "Yanlış sözler mi?" / "Sözleri ara"
        return true;
    }
    if (shownTab_ == Tab::Lyrics && state_ == LyricsState::Synced) {
        const int i = lineAt(lp);
        if (i >= 0 && contentRect_.contains(lp) && lyrics_.lines[i].timeMs >= 0) {
            seekToLyricsTime(trackId_, lyrics_.lines[i].timeMs);
            manualUntil_ = 0;
            return true;
        }
    }
    return false;
}

void NowPlayingView::onMouseMove(const ui::MouseEvent& e) {
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    const int line = shownTab_ == Tab::Lyrics && state_ == LyricsState::Synced && contentRect_.contains(lp) ? lineAt(lp) : -1;
    int sub = -1;
    for (size_t i = 0; i < subLinkRects_.size(); ++i)
        if (subLinkRects_[i].contains(lp)) sub = static_cast<int>(i);
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    const bool titleLink = t && (radio::isStationId(t->id) || catalog::isPodcastId(t->id) || !t->album.id.empty());
    const bool title = titleLink && titleRect_.contains(lp) && sub < 0;
    const bool match = matchRect_.contains(lp);
    const bool picker = shownTab_ == Tab::Lyrics && kind() == Kind::Song && pickerRect_.contains(lp);
    int link = -1;
    if (shownTab_ == Tab::About)
        for (size_t i = 0; i < links_.size(); ++i)
            if (links_[i].r.contains(lp)) link = static_cast<int>(i);
    if (line != hoverLine_ || sub != hoverSub_ || title != hoverTitle_ || match != hoverMatch_ || link != hoverLink_ ||
        picker != hoverPicker_) {
        hoverLine_ = line;
        hoverSub_ = sub;
        hoverTitle_ = title;
        hoverMatch_ = match;
        hoverLink_ = link;
        hoverPicker_ = picker;
        invalidate();
    }
}

void NowPlayingView::onMouseLeave() {
    hoverLine_ = hoverSub_ = hoverLink_ = -1;
    hoverTitle_ = hoverMatch_ = hoverPicker_ = false;
    invalidate();
}

LPCWSTR NowPlayingView::cursor() const {
    return (hoverLine_ >= 0 || hoverSub_ >= 0 || hoverTitle_ || hoverMatch_ || hoverLink_ >= 0 || hoverPicker_) ? IDC_HAND
                                                                                                                 : IDC_ARROW;
}

} // namespace st::app
