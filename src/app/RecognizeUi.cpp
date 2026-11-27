// "Şarkıyı tanı": the dialog, its states and the entry points (see Recognize.h).
#include "app/Recognize.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/Components.h"
#include "app/PageWidgets.h"
#include "app/Radio.h"
#include "app/Recognition.h"
#include "app/Source.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "recognize/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/Layout.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace st::app {

namespace {

using gfx::accent;
using gfx::colors;
using recognize::Failure;
using recognize::Phase;
using recognize::Song;
using recognize::Source;
namespace type = gfx::type;

constexpr char kTag[] = "recognize";
constexpr float kStageH = 236;
constexpr int kBars = 56;
constexpr double kSampleMs = 40;   // one bar of the wave
constexpr double kMaxHeard = 12;   // the session's longest signature (the progress line's end)

struct Module {
    recognition::History history;
    bool loaded = false;
    class Body* body = nullptr;    // the open dialog's content
};
Module& mod() {
    static Module m;
    return m;
}

recognition::History& history() {
    auto& m = mod();
    if (!m.loaded) {
        m.loaded = true;
        m.history.load(paths::appData() / L"recognized.json");
    }
    return m.history;
}

// Worker thread: the recognized song as a track of the active catalog (the first search hit that is the song), else
// the song by name (matched on YouTube when played).
catalog::Track resolveTrack(spotify::Api* api, bool ytm, const Song& song) {
    const std::string query = recognition::searchQuery(song);
    std::vector<catalog::Track> hits;
    try {
        if (api) {
            hits = api->search(query, 10).tracks;
        } else if (ytm) {
            hits = source::quickSearch(nullptr, query, 10).tracks;
        } else {
            if (!song.isrc.empty()) hits = mb::searchTracks("isrc:" + song.isrc, 10);
            if (recognition::bestMatch(hits, song) < 0) hits = mb::searchTracks(query, 10);
        }
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "catalog search for the recognized song failed: {}", e.what());
    }
    const int best = recognition::bestMatch(hits, song);
    if (best < 0) {
        ST_LOG_INFO(kTag, "the recognized song is not in the catalog ({} hits): played by name", hits.size());
        return recognition::fallbackTrack(song);
    }
    catalog::Track t = hits[static_cast<size_t>(best)];
    if (t.album.images.empty() && !song.coverUrl.empty()) t.album.images.push_back({song.coverUrl, 400, 400});
    ST_LOG_INFO(kTag, "recognized song -> {}", t.id);
    return t;
}

// The wave's height for a block peak: -50 dBFS (and below) is the floor, -6 dBFS the top.
float levelHeight(float peak) {
    if (peak <= 1e-5f) return 0;
    const float db = 20 * std::log10(peak);
    return std::clamp((db + 50) / 44, 0.f, 1.f);
}

bool shadeTubePlaying() {
    auto* p = ctx().player;
    return p && p->current() && p->isPlaying();
}

// The recognized song is the one ShadeTube plays right now.
bool isPlayingSong(const Song& s) {
    auto* p = ctx().player;
    const catalog::Track* cur = p ? p->current() : nullptr;
    if (!cur || recognition::foldTitle(cur->name) != recognition::foldTitle(s.title)) return false;
    return recognition::bestMatch({*cur}, s) == 0;
}

// ---- a history row ------------------------------------------------------------------------------------------------------

class HistoryRow : public ui::Widget {
public:
    explicit HistoryRow(const recognition::Entry& e) : entry_(e) {
        focusable = true;
        title_ = gfx::Text(toWide(e.song.title), type::body);
        artist_ = gfx::Text(toWide(e.song.artist), type::caption);
        gfx::TextOptions right;
        right.align = gfx::TextAlign::Trailing;
        when_ = gfx::Text(relativeTime(e.at), type::monoMeta, right);
        images_ = e.track.album.images;
        if (images_.empty() && !e.song.coverUrl.empty()) images_.push_back({e.song.coverUrl, 400, 400});
    }
    std::function<void(const recognition::Entry&)> onPick;
    float preferredHeight(float) override { return 52; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        pick();
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (rect().contains(e.pos)) pick();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override { hover_.to(0, ui::motion::microMs(ui::motion::fast)); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hover_));
        drawArtwork(c, images_, {r.x + 8, r.cy() - 18, 36, 36}, gfx::radii().art);
        const float x = r.x + 8 + 36 + 12;
        const float ww = std::ceil(when_.measure().w);
        const float w = std::max(40.f, r.right() - 12 - ww - 12 - x);
        c.text(title_, {x, r.y + 8, w, 20}, col.fgPrimary, gfx::VAlign::Center);
        c.text(artist_, {x, r.y + 27, w, 17}, col.fgSecondary, gfx::VAlign::Center);
        c.text(when_, {r.right() - 12 - ww, r.y, ww, r.h}, col.fgTertiary, gfx::VAlign::Center);
    }

private:
    void pick() {
        if (!onPick) return;
        const auto fn = onPick;
        fn(entry_);
    }
    recognition::Entry entry_;
    gfx::Text title_, artist_, when_;
    std::vector<catalog::Image> images_;
    ui::Anim hover_;
};

// ---- the dialog's content -------------------------------------------------------------------------------------------------

class Body;

// The stage: listening wave / result card / state message. Painted by the Body; holds the result's buttons and the
// message panel as children, so they come right after the source switch in the Tab order.
class Stage : public ui::Widget {
public:
    explicit Stage(Body* b) : body_(b) {}
    void paint(Canvas& c) override;
    void layout() override;

private:
    Body* body_;
};

class Body : public ui::Widget {
public:
    explicit Body(ui::Dialog* d) : dialog_(d) {
        mod().body = this;
        sys_ = add<ui::Button>(ui::ButtonKind::Chip, tr(L"Bilgisayarın sesi"));
        sys_->setTooltip(tr(L"Bu bilgisayarda çalan sesi dinle: tarayıcıdaki bir video, bir oyun, bir yayın"));
        sys_->onClick = [this] { pickSource(Source::System); };
        mic_ = add<ui::Button>(ui::ButtonKind::Chip, tr(L"Mikrofon"));
        mic_->setTooltip(tr(L"Odada çalan müziği mikrofonla dinle"));
        mic_->onClick = [this] { pickSource(Source::Microphone); };
        stage_ = add<Stage>(this);
        // Reading order (Tab): the card's link under the title, then the buttons under the cover.
        retry_ = stage_->add<ui::Button>(ui::ButtonKind::Link, tr(L"Yanlış mı? Tekrar dene"));
        retry_->onClick = [this] { Dispatcher::post([this, ref = life_.ref()] {
            if (!ref.expired()) start(source_);
        }); };
        play_ = stage_->add<ui::Button>(ui::ButtonKind::Primary, tr(L"Çal"), "play");
        play_->onClick = [this] { playSong(); };
        like_ = stage_->add<ui::Button>(ui::ButtonKind::IconOutline, std::wstring(), "heart");
        like_->setHeart("heart", "heart-filled");
        like_->onClick = [this] { toggleLike(); };
        add_ = stage_->add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Listeye ekle"), "playlist");
        add_->onClick = [this] { addToList(); };
        radio_ = stage_->add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Radyo"), "radio");
        radio_->onClick = [this] { startSongRadio(); };
        spinner_ = stage_->add<ui::Spinner>(18.f);
        clear_ = add<ui::Button>(ui::ButtonKind::Link, tr(L"Temizle"));
        clear_->setTooltip(tr(L"Tanınan şarkıların listesini sil"));
        clear_->onClick = [this] { clearHistory(); };
        scroll_ = add<ui::ScrollView>();
        scroll_->barAlways = true;   // a short list in a dialog: shows there is more
        list_ = scroll_->setContent<ui::Column>(2.f);
        privacy_ = gfx::Text(tr(L"Ses bu bilgisayardan çıkmaz; tanıma için yalnızca sesin parmak izi gönderilir."), type::caption);
        histHeader_ = gfx::Text(toUpperTr(tr(L"Son tanınanlar")), type::monoLabel);
        gfx::TextOptions centered;
        centered.align = gfx::TextAlign::Center;
        histEmpty_ = gfx::Text(tr(L"Tanıdığın şarkılar burada listelenir."), type::secondary, centered);
        source_ = history().source();
        rebuildHistory();
        showResultButtons(false);
    }
    ~Body() override {
        if (mod().body == this) mod().body = nullptr;
        life_.renew();
        session_.reset();
    }

    // (Re)starts listening on `s`.
    void start(Source s) {
        life_.renew();
        session_.reset();
        source_ = s;
        sys_->setActive(s == Source::System);
        mic_->setActive(s == Source::Microphone);
        view_ = View::Listening;
        phase_ = Phase::Listening;
        heard_ = 0;
        requests_ = 0;
        ownIncluded_ = false;
        confirming_ = false;
        levels_.assign(kBars, 0.f);
        lastSample_ = ui::frame::now();
        startedAt_ = lastSample_;
        clearMessage();
        showResultButtons(false);
        recognize::Options o;
        o.source = s;
        auto ref = life_.ref();
        ST_LOG_INFO(kTag, "listening to the {}", s == Source::System ? "system sound" : "microphone");
        if (!demoListen_)
            session_ = std::make_unique<recognize::Session>(o, [this, ref](const recognize::Event& e) {
                Dispatcher::post([this, ref, e] {
                    if (!ref.expired()) onEvent(e);
                });
            });
        updateStatus();
        requestLayout();
        invalidate();
    }

    void stopListening() {
        life_.renew();
        session_.reset();
    }

    // A history row: its card (the track it was resolved to, else resolved now).
    void showEntry(const recognition::Entry& e) {
        stopListening();
        showSong(e.song, e.track.id.empty() ? std::nullopt : std::optional<catalog::Track>(e.track), false);
    }

    // Dev (SHADETUBE_RECOGNIZE_DEMO): a state without listening.
    void demo(const std::string& state) {
        stopListening();
        if (state == "listening" || state == "identifying") {
            demoListen_ = true;
            start(source_);
            demoListen_ = false;
            demoLevels_ = true;
            heard_ = state == "identifying" ? 4.2 : 2.6;
            requests_ = state == "identifying" ? 1 : 0;
            phase_ = state == "identifying" ? Phase::Identifying : Phase::Listening;
            ownIncluded_ = false;
            updateStatus();
            invalidate();
            return;
        }
        if (state.rfind("result", 0) == 0) {
            Song s;
            s.title = "Şımarık";
            s.artist = "Tarkan";
            s.album = "Ölürüm Sana";
            s.year = "1997";
            s.genre = "Turkish Pop";
            s.isrc = "TR1290601381";
            s.coverUrl = state.size() > 7 ? state.substr(7) : std::string();
            showSong(s, std::nullopt, true);
            return;
        }
        recognize::Event e;
        e.phase = state == "notfound" ? Phase::NotFound : state == "silent" ? Phase::Silent : Phase::Failed;
        if (state.rfind("failed:", 0) == 0) {
            const std::string k = state.substr(7);
            e.failure = k == "access" ? Failure::AccessDenied : k == "busy" ? Failure::DeviceBusy : k == "device" ? Failure::NoDevice
                        : k == "rate" ? Failure::RateLimited : k == "service" ? Failure::Service : Failure::Network;
        }
        onEvent(e);
    }

    // Dev: the result card's buttons (SHADETUBE_RECOGNIZE_DEMO act=play|like|radio).
    void demoAct(const std::string& what) {
        if (what == "play") playSong();
        else if (what == "like") toggleLike();
        else if (what == "radio") startSongRadio();
    }

    void paintStage(Canvas& c, const Rect& r);
    void layoutStage(const Rect& r);

    float preferredHeight(float) override { return 36 + 12 + kStageH + 12 + 18 + 22 + 20 + 8 + 52 * 2 + 2; }   // two history rows (the list scrolls)

    void layout() override {
        const Rect r = rect();
        float x = 0;
        for (auto* b : {sys_, mic_}) {
            const float w = std::ceil(b->naturalWidth());
            b->setRect({x, 0, w, 36});
            x += w + 8;
        }
        stage_->setRect({0, 48, r.w, kStageH});
        const float hy = 48 + kStageH + 12 + 18 + 22;
        const float cw = std::ceil(clear_->naturalWidth());
        clear_->setRect({r.w - cw, hy - 4, cw, 24});
        clear_->setVisible(!history().entries().empty());
        const float ly = hy + 20 + 8;
        scroll_->setRect({0, ly, r.w, std::max(0.f, r.h - ly)});
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        paintChildren(c);
        // The privacy line under the stage.
        const float py = r.y + 48 + kStageH + 12;
        c.icon("lock", {r.x, py + 1, 14, 14}, col.fgTertiary);
        c.text(privacy_, {r.x + 20, py, r.w - 20, 16}, col.fgTertiary, gfx::VAlign::Center);
        // The history's header.
        const float hy = py + 18 + 22;
        c.text(histHeader_, {r.x, hy, r.w * 0.6f, 16}, col.fgTertiary, gfx::VAlign::Center);
        if (history().entries().empty()) {
            const Rect lr = scroll_->rect();
            c.text(histEmpty_, {r.x, r.y + lr.y + 16, r.w, 20}, col.fgTertiary, gfx::VAlign::Center);
        }
    }

private:
    enum class View { Listening, Result, Message };
    friend class Stage;

    void pickSource(Source s) {
        auto& h = history();
        if (h.source() != s) {
            h.setSource(s);
            h.save();
        }
        Dispatcher::post([this, s, ref = life_.ref()] {
            if (!ref.expired()) start(s);
        });
    }

    void onEvent(const recognize::Event& e) {
        heard_ = e.heardSec;
        requests_ = e.requests;
        ownIncluded_ = e.ownSoundIncluded;
        confirming_ = e.confirming;
        phase_ = e.phase;
        if (!e.final()) {
            updateStatus();
            invalidate();
            return;
        }
        session_.reset();   // finished: the capture has stopped already
        if (e.phase == Phase::Matched && e.song) {
            recognition::Entry en;
            en.at = nowUnix();
            en.song = *e.song;
            auto& h = history();
            h.add(en);
            h.save();
            rebuildHistory();
            showSong(*e.song, std::nullopt, true);
            return;
        }
        showMessage(e);
    }

    void updateStatus() {
        const bool mic = source_ == Source::Microphone;
        status_ = gfx::Text(phase_ == Phase::Identifying ? tr(L"Tanınıyor…")
                            : confirming_                ? tr(L"Emin olmak için biraz daha dinleniyor…")
                            : requests_ > 0              ? tr(L"Biraz daha dinleniyor…")
                                                         : tr(L"Dinleniyor…"),
                            type::sectionTitle, centeredOptions());
        std::wstring line = mic ? tr(L"Odada çalan müzik mikrofondan dinleniyor") : tr(L"Bu bilgisayarda çalan ses dinleniyor");
        noteWarn_ = false;
        if (!mic && shadeTubePlaying()) {
            if (ownIncluded_) {
                line = tr(L"ShadeTube de çalıyor: onun sesi de duyuluyor. Duraklat ya da mikrofonu seç.");
                noteWarn_ = true;
            } else {
                line = tr(L"ShadeTube'un kendi sesi dinlenmez, yalnızca diğer uygulamalarınki.");
            }
        }
        line_ = gfx::Text(line, type::secondary, centeredOptions(true));
    }

    static gfx::TextOptions centeredOptions(bool wrap = false) {
        gfx::TextOptions o;
        o.align = gfx::TextAlign::Center;
        o.wrap = wrap;
        o.maxLines = wrap ? 2 : 0;
        return o;
    }

    void showSong(const Song& s, std::optional<catalog::Track> track, bool fresh) {
        view_ = View::Result;
        song_ = s;
        clearMessage();
        title_ = gfx::Text(toWide(s.title), type::title, wrapOptions(2));
        artist_ = gfx::Text(toWide(s.artist), type::body);
        std::wstring meta = toWide(s.album);
        for (const auto& part : {s.year, s.genre})
            if (!part.empty()) meta += (meta.empty() ? L"" : L" · ") + toWide(part);
        meta_ = gfx::Text(meta, type::caption);
        note_ = gfx::Text(isPlayingSong(s) ? tr(L"Bu şarkı şu an ShadeTube'da çalıyor") : std::wstring(), type::caption);
        coverFallback_.clear();
        if (!s.coverUrl.empty()) coverFallback_.push_back({s.coverUrl, 400, 400});
        resolved_ = track.has_value();
        if (track) track_ = *track;
        else track_ = recognition::fallbackTrack(s);
        showResultButtons(true);
        syncButtons();
        if (!resolved_) resolve(fresh);
        requestLayout();
        invalidate();
    }

    static gfx::TextOptions wrapOptions(int lines) {
        gfx::TextOptions o;
        o.wrap = true;
        o.maxLines = lines;
        return o;
    }

    void resolve(bool fresh) {
        spotify::Api* api = source::activeApi();
        const bool ytm = source::ytmMode();
        const Song s = song_;
        async(Priority::High, life_.ref(), [api, ytm, s] { return resolveTrack(api, ytm, s); },
              [this, s, fresh](Result<catalog::Track> r) {
                  if (view_ != View::Result || song_.title != s.title || song_.artist != s.artist) return;
                  track_ = r ? *r : recognition::fallbackTrack(s);
                  resolved_ = true;
                  auto& h = history();
                  h.setTrack(s, track_);
                  h.save();
                  if (fresh) rebuildHistory();
                  syncButtons();
                  invalidate();
              });
    }

    void showResultButtons(bool on) {
        for (auto* b : {play_, like_, add_, radio_, retry_}) b->setVisible(on);
        spinner_->setVisible(false);
    }

    void syncButtons() {
        const bool ready = resolved_;
        spinner_->setVisible(!ready);
        play_->setEnabled(ready);
        add_->setEnabled(ready);
        auto& lib = ctx().library;
        const bool canLike = ready && lib.canLike(track_.id);
        like_->setEnabled(canLike);
        like_->setActive(ready && lib.isLiked(track_.id));
        like_->setTooltip(!ready     ? tr(L"Beğen")
                          : canLike  ? (lib.isLiked(track_.id) ? tr(L"Beğenilenlerden kaldır") : tr(L"Beğenilen Şarkılar'a ekle"))
                                     : tr(L"Bu şarkı Spotify'da bulunamadı: beğenilemiyor"));
        const bool radio = ready && radioAvailable() && !radioSeed(track_.id).empty();
        radio_->setEnabled(radio);
        radio_->setTooltip(radio ? tr(L"Bu şarkının radyosunu başlat")
                                 : tr(L"Radyo, Spotify'da ya da YouTube Music'te bulunan şarkılar için var"));
        add_->setTooltip(tr(L"Çalma listesine ekle"));
        play_->setTooltip(tr(L"Şarkıyı çal"));
        requestLayout();
    }

    void playSong() {
        auto* p = ctx().player;
        if (!p || !resolved_) return;
        const catalog::Track t = track_;
        auto* d = dialog_;
        p->playContext({t}, 0, {"recognized", tr(L"Tanınan şarkı")});
        if (d) d->close();
    }

    void toggleLike() {
        if (!resolved_) return;
        ctx().library.toggleLiked(track_);
        like_->setActive(ctx().library.isLiked(track_.id));
        like_->pulse();
        syncButtons();
    }

    void addToList() {
        if (!resolved_ || !window()) return;
        const Rect r = add_->toWindow(add_->rect());
        ui::Menu::open(window(), {r.x, r.bottom() + 4}, addToPlaylistItems({track_}));
    }

    void startSongRadio() {
        if (!resolved_) return;
        const catalog::Track t = track_;
        auto* d = dialog_;
        startRadio(t.id, toWide(t.name));
        if (d) d->close();
    }

    void clearHistory() {
        auto& h = history();
        h.clear();
        h.save();
        rebuildHistory();
        Dispatcher::post([] {   // after the click: the link goes away with it
            if (auto* b = mod().body) b->requestLayout();
        });
    }

    void rebuildHistory() {
        list_->clearChildren();
        for (const auto& e : history().entries()) {
            auto* row = list_->add<HistoryRow>(e);
            row->onPick = [this](const recognition::Entry& en) {
                Dispatcher::post([this, en, ref = life_.ref()] {
                    if (!ref.expired()) showEntry(en);
                });
            };
        }
        scroll_->contentChanged();
        requestLayout();
        invalidate();
    }

    void clearMessage() {
        if (message_) stage_->removeChild(message_);
        message_ = nullptr;
    }

    void showMessage(const recognize::Event& e) {
        view_ = View::Message;
        clearMessage();
        showResultButtons(false);
        const bool mic = source_ == Source::Microphone;
        std::string icon = "recognize";
        std::wstring title, line, action = tr(L"Tekrar dene");
        std::string actionIcon;   // "" = the panel's default (none, or refresh for a failure)
        std::function<void()> act = [this, ref = life_.ref()] {
            Dispatcher::post([this, ref] {
                if (!ref.expired()) start(source_);
            });
        };
        auto tone = MessagePanel::Tone::Empty;
        switch (e.phase) {
        case Phase::NotFound:
            title = tr(L"Şarkı tanınamadı");
            line = mic ? tr(L"Mikrofonu müziğe yaklaştırıp şarkının belirgin bir yerinde tekrar dene.")
                       : tr(L"Sesi biraz açıp şarkının belirgin bir yerinde tekrar dene.");
            icon = "search";
            break;
        case Phase::Silent:
            title = mic ? tr(L"Mikrofondan ses gelmiyor") : tr(L"Bilgisayarda çalan bir ses yok");
            line = mic ? tr(L"Mikrofonun bağlı ve sessize alınmamış olduğundan emin ol.")
                       : tr(L"Bir video, oyun ya da yayın çalarken tekrar dene ya da mikrofonu seç. ShadeTube'un kendi sesi dinlenmez.");
            icon = mic ? "podcast" : "volume-0";
            break;
        default:
            tone = MessagePanel::Tone::Error;
            icon = "error";
            switch (e.failure) {
            case Failure::NoDevice:
                title = mic ? tr(L"Mikrofon bulunamadı") : tr(L"Ses çıkışı bulunamadı");
                line = tr(L"Bir ses aygıtı bağlayıp tekrar dene.");
                break;
            case Failure::DeviceBusy:
                title = tr(L"Ses aygıtı başka bir uygulamada");
                line = tr(L"Bir uygulama aygıtı özel modda kullanıyor. O bırakınca tekrar dene ya da mikrofonu seç.");
                break;
            case Failure::AccessDenied:
                title = tr(L"Mikrofon izni kapalı");
                line = tr(L"Windows Ayarları › Gizlilik ve güvenlik › Mikrofon'da masaüstü uygulamalarının mikrofona erişimini aç.");
                action = tr(L"Ayarları aç");
                actionIcon = "external-link";
                act = [] { ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-microphone", nullptr, nullptr, SW_SHOWNORMAL); };
                break;
            case Failure::Network:
                title = tr(L"Tanıma hizmetine ulaşılamadı");
                line = tr(L"İnternet bağlantını kontrol edip tekrar dene.");
                tone = MessagePanel::Tone::Offline;
                icon = "offline";
                break;
            case Failure::RateLimited:
                title = tr(L"Çok sık denendi");
                line = tr(L"Tanıma hizmeti kısa bir mola istiyor. Bir dakika sonra tekrar dene.");
                break;
            case Failure::Service:
                title = tr(L"Tanıma hizmeti yanıt vermedi");
                line = tr(L"Biraz sonra tekrar dene.");
                break;
            case Failure::Capture:
            case Failure::None:
                title = tr(L"Ses dinlenemedi");
                line = tr(L"Ses aygıtı açılamadı. Tekrar dene ya da diğer kaynağı seç.");
                break;
            }
        }
        message_ = stage_->add<MessagePanel>(icon, title, line, action, act);
        message_->setTone(tone).setCompact(true);
        if (!actionIcon.empty()) message_->setActionIcon(actionIcon);
        stage_->requestLayout();
        invalidate();
    }

    ui::Dialog* dialog_;
    ui::Button *sys_, *mic_;
    Stage* stage_;
    ui::Button *play_, *like_, *add_, *radio_, *retry_, *clear_;
    ui::Spinner* spinner_;
    MessagePanel* message_ = nullptr;
    ui::ScrollView* scroll_;
    ui::Column* list_;
    std::unique_ptr<recognize::Session> session_;
    Lifetime life_;
    Source source_ = Source::System;
    View view_ = View::Listening;
    Phase phase_ = Phase::Listening;
    double heard_ = 0;
    int requests_ = 0;
    bool ownIncluded_ = false, noteWarn_ = false, confirming_ = false;
    bool demoListen_ = false, demoLevels_ = false;
    std::vector<float> levels_;
    double lastSample_ = 0, startedAt_ = 0;
    Song song_;
    catalog::Track track_;
    bool resolved_ = false;
    std::vector<catalog::Image> coverFallback_;
    gfx::Text privacy_, histHeader_, histEmpty_, status_, line_, title_, artist_, meta_, note_;
};

void Stage::paint(Canvas& c) {
    body_->paintStage(c, rect());
    paintChildren(c);
}

void Stage::layout() { body_->layoutStage(rect()); }

void Body::layoutStage(const Rect& r) {
    // Local to the stage.
    if (message_) message_->setRect({1, 1, r.w - 2, r.h - 2});
    const float pad = 24, art = 128;
    const float tx = pad + art + 20;
    const float by = pad + art + 20;
    float x = pad;
    for (auto* b : {play_, like_, add_, radio_}) {
        const float w = b == like_ ? 40.f : std::max(b == play_ ? 88.f : 0.f, std::ceil(b->naturalWidth()));
        b->setRect({x, by, w, 40});
        x += w + 8;
    }
    spinner_->setRect({x + 4, by + 11, 18, 18});
    const float rw = std::ceil(retry_->naturalWidth());
    retry_->setRect({tx, pad + art - 26, std::min(rw, r.w - tx), 28});
}

void Body::paintStage(Canvas& c, const Rect& r) {
    const auto& col = colors();
    c.strokeRounded(r, gfx::radii().surface, col.hairDefault);
    if (view_ == View::Message) return;
    if (view_ == View::Result) {
        const float pad = 24, art = 128;
        const Rect cover{r.x + pad, r.y + pad, art, art};
        drawArtwork(c, track_.album.images.empty() ? coverFallback_ : track_.album.images, cover, gfx::radii().art);
        const float x = r.x + pad + art + 20, w = r.right() - pad - x;
        float y = r.y + pad + 2;
        const float th = std::ceil(title_.measure(w).h);
        c.text(title_, {x, y, w, th}, col.fgPrimary);
        y += th + 4;
        c.text(artist_, {x, y, w, 20}, col.fgSecondary, gfx::VAlign::Center);
        y += 22;
        if (!meta_.empty()) {
            c.text(meta_, {x, y, w, 18}, col.fgTertiary, gfx::VAlign::Center);
            y += 20;
        }
        if (!note_.empty()) c.text(note_, {x, y, w, 18}, accent().base, gfx::VAlign::Center);
        return;
    }
    // Listening: the wave of the last block peaks (newest on the right), the status, the progress line.
    const double now = ui::frame::now();
    const bool micro = ui::motion::micro();
    const double step = micro ? kSampleMs : 250.0;
    float live = session_ ? session_->level() : 0.f;
    if (demoLevels_) {   // dev: a made-up wave for screenshots
        const double t = now / 1000.0;
        live = static_cast<float>(0.25 + 0.2 * std::sin(t * 7.3) + 0.15 * std::sin(t * 2.1 + 1) + 0.1 * std::sin(t * 17.0));
    }
    if (now - lastSample_ > 1000) lastSample_ = now - step;   // back from a pause in painting: no burst of bars
    while (now - lastSample_ >= step) {
        lastSample_ += step;
        if (micro) {
            levels_.erase(levels_.begin());
            levels_.push_back(levelHeight(live));
        } else {
            std::fill(levels_.begin(), levels_.end(), levelHeight(live));
        }
    }
    const float waveH = 76, waveW = std::min(r.w - 64, 420.f);
    const float cy = r.y + 24 + waveH / 2;
    const float barW = 3, gap = (waveW - kBars * barW) / (kBars - 1);
    const float x0 = std::round(r.cx() - waveW / 2);
    for (int i = 0; i < kBars; ++i) {
        float l = levels_[static_cast<size_t>(i)];
        if (!micro) {   // a still shape for Hareketi azalt: the current level, highest in the middle
            const float d = std::abs(i - (kBars - 1) * 0.5f) / (kBars * 0.5f);
            l *= 1 - 0.6f * d * d;
        }
        const float h = std::max(3.f, l * waveH);
        const float alpha = micro ? 0.3f + 0.7f * static_cast<float>(i) / (kBars - 1) : 1.f;
        c.fillPill({x0 + i * (barW + gap), cy - h / 2, barW, h}, accent().base.mulAlpha(alpha));
    }
    const float sy = r.y + 24 + waveH + 18;
    c.text(status_, {r.x + 16, sy, r.w - 32, 24}, col.fgPrimary, gfx::VAlign::Center);
    c.text(line_, {r.x + 32, sy + 28, r.w - 64, 36}, noteWarn_ ? col.warning : col.fgSecondary);
    // Progress through the 12 s the session listens at most.
    const float px0 = r.x + 20, pw = r.w - 40, py = r.bottom() - 14;
    c.hline(px0, px0 + pw, py, col.hairDefault, 2);
    const float f = static_cast<float>(std::clamp(heard_ / kMaxHeard, 0.0, 1.0));
    if (f > 0) c.hline(px0, px0 + pw * f, py, accent().base, 2);
    if (auto* w = window()) w->invalidateAfter(micro ? kSampleMs : 250.0);
    // The time the session itself reports moves in steps; between events the line follows the clock.
    if (session_ && phase_ == Phase::Listening)
        heard_ = std::max(heard_, std::min(kMaxHeard, session_->heardSec()));
}

void openDialog() {
    auto* w = ctx().window;
    if (!w) return;
    if (auto* b = mod().body) {   // already open: listen again
        b->start(history().source());
        return;
    }
    auto* d = ui::Dialog::open(w, tr(L"Şarkıyı tanı"), {}, 560);
    if (!d) return;
    auto* body = d->body()->add<Body>(d);
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    d->onClosed = [] {
        if (auto* b = mod().body) b->stopListening();   // the capture stops with the dialog
    };
    body->start(history().source());
}

// Dev: SHADETUBE_RECOGNIZE_DEMO = "<action>@<ms>[=<arg>];..." after startup: open[=system|mic], state=<listening |
// identifying | result[:<cover url>] | notfound | silent | failed:<access|busy|device|network|rate|service>>,
// history=<n> (sample entries), act=<play|like|radio> (the result card's buttons), close.
void startDemo() {
    wchar_t buf[1024] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_RECOGNIZE_DEMO", buf, 1024)) return;
    const std::string spec = toUtf8(buf);
    size_t start = 0;
    while (start < spec.size()) {
        const size_t end = std::min(spec.find(';', start), spec.size());
        const std::string step = spec.substr(start, end - start);
        start = end + 1;
        const size_t at = step.find('@');
        if (at == std::string::npos) continue;
        const std::string action = step.substr(0, at);
        const size_t eq = step.find('=', at);
        const int ms = std::atoi(step.substr(at + 1, eq == std::string::npos ? std::string::npos : eq - at - 1).c_str());
        const std::string arg = eq == std::string::npos ? std::string() : step.substr(eq + 1);
        auto run = [action, arg] {
            if (action == "open") {
                if (arg == "mic") history().setSource(Source::Microphone);
                else if (arg == "system") history().setSource(Source::System);
                openDialog();
            } else if (action == "state") {
                if (!mod().body) openDialog();
                if (auto* b = mod().body) b->demo(arg);
            } else if (action == "history") {
                const char* songs[][3] = {{"Gülümse", "Sezen Aksu", "Gülümse"}, {"Take Five", "Dave Brubeck", "Time Out"},
                                          {"One More Time", "Daft Punk", "Discovery"}, {"Bohemian Rhapsody", "Queen", "A Night At The Opera"},
                                          {"Lose Yourself", "Eminem", "8 Mile"}};
                const int n = std::clamp(std::atoi(arg.c_str()), 0, 5);
                for (int i = n - 1; i >= 0; --i) {
                    recognition::Entry e;
                    e.at = nowUnix() - (i + 1) * 3600 * (i + 1);
                    e.song.title = songs[i][0];
                    e.song.artist = songs[i][1];
                    e.song.album = songs[i][2];
                    history().add(e);
                }
            } else if (action == "act") {
                if (auto* b = mod().body) b->demoAct(arg);
            } else if (action == "close") {
                if (auto* b = mod().body) b->stopListening();
            }
        };
        std::thread([ms, run] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            Dispatcher::post(run);
        }).detach();
    }
}

} // namespace

void openSongRecognition() {
    Dispatcher::post([] { openDialog(); });
}

void initSongRecognition() {
    commands::setHandler("recognize-song", [] {
        // A global key while the window is away: bring it up first (show-window toggles, so only when it is not in front).
        if (auto* w = ctx().window) {
            const HWND h = w->hwnd();
            if (!IsWindowVisible(h) || IsIconic(h) || GetForegroundWindow() != h) commands::run("show-window");
            // Still hidden: the mini player was brought up instead (it stands for the main window). The dialog lives in
            // the main window, so the mini player closes (its toggle shows the main window again).
            if (!IsWindowVisible(h)) commands::run("mini-player");
        }
        openSongRecognition();
        return true;
    });
    // App exit: no capture or request outlives the window.
    ctx().persistHooks.push_back([] {
        if (auto* b = mod().body) b->stopListening();
    });
    startDemo();
}

} // namespace st::app
