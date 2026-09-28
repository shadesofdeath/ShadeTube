// İstatistikler (Spotube's Stats): what the user really listened to over the last 7 / 30 days or all time —
// total time, streams, distinct songs / artists, top songs / artists / albums and recent plays. The data comes from
// app/ListenStats, which initListenStats() feeds from the player through the AppContext hooks (every source:
// Spotify, MusicBrainz, downloads, local files; not internet radio stations).
#include "app/AppContext.h"
#include "app/InternetRadio.h"
#include "app/ListenStats.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SettingsWidgets.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <unordered_map>
#include <unordered_set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;

namespace {

ListenStats& store() {
    static ListenStats s;   // %LOCALAPPDATA%\ShadeTube\listening.json
    return s;
}

// ---- recording ----------------------------------------------------------------------------------------------

void sample();

// While a play is running the store is also sampled from a 2 s thread timer: Win32 modal loops (dragging / resizing
// the window, the tray menu) dispatch it while App's housekeeping stands still, so the stretch credited at a track
// change (capped at kGapCreditMs) never has to cover more than ~2 s. Off while paused / stopped.
UINT_PTR g_sampleTimer = 0;

void CALLBACK onSampleTimer(HWND, UINT, UINT_PTR, DWORD) { sample(); }

void syncSampleTimer(bool on) {
    if (on && !g_sampleTimer) {
        g_sampleTimer = SetTimer(nullptr, 0, 2000, onSampleTimer);
    } else if (!on && g_sampleTimer) {
        KillTimer(nullptr, g_sampleTimer);
        g_sampleTimer = 0;
    }
}

// Feeds the player's state to the store: on every player change, from housekeeping and from the timer above.
void sample() {
    auto& s = store();
    const auto* p = ctx().player;
    if (!p || !s.active()) {
        syncSampleTimer(false);
        return;
    }
    const int64_t now = steadyMs();
    const Track* cur = p->current();
    const auto status = p->status();
    if (!cur || status == player::Status::Idle) {   // end of the queue is Idle (current() is kept): the play is over
        s.stop(now);
        syncSampleTimer(false);
        return;
    }
    // Another track is already current: its trackChanged hook comes next and closes this play (a local file notifies
    // before it). While resolving, the position is 0 or the pending seek target, not a sample.
    if (cur->id != s.currentTrackId() || cur->name != s.currentTrackName() || status == player::Status::Resolving) return;
    s.progress(p->positionMs(), status == player::Status::Playing, p->durationMs(), now, nowUnix());
    syncSampleTimer(status == player::Status::Playing);
}

int64_t g_lastSaveMs = 0;

// ---- page helpers ---------------------------------------------------------------------------------------------

StatsPeriod g_period = StatsPeriod::Month;   // kept while the app runs

std::wstring durationLabel(int64_t ms) {   // "45 sn" / "43 dk" / "12 sa 40 dk"
    if (ms < 60'000) return i18n::format(tr(L"{} sn"), std::max<int64_t>(0, ms) / 1000);
    return totalDuration(ms);
}

std::wstring perItem(int streams, int items) {   // "1,9" / "1.9" (the UI language's decimal separator)
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", items > 0 ? static_cast<double>(streams) / items : 0.0);
    std::wstring s = b;
    std::replace(s.begin(), s.end(), L'.', i18n::decimalSeparator());
    return s;
}

std::wstring twoDigits(size_t n) {
    wchar_t b[16];
    swprintf(b, 16, L"%02zu", n);
    return b;
}

// Only ids the detail pages can open: MusicBrainz MBIDs, and Spotify URIs of that kind while logged in (logged out they
// would reach MusicBrainz and fail). Local files have their own ids. The rest opens a search.
bool isMbid(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23 ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}
bool routable(const std::string& id, std::string_view spotifyPrefix) {
    return (id.rfind(spotifyPrefix, 0) == 0 && source::loggedIn()) || isMbid(id);
}

// Artist pictures. Tracks carry none: a followed artist's comes from the library (Spotify or local); the others are
// looked up (see fetchArtistImages). Answers are cached for the session ({} = the source has none); a failed lookup
// (offline, 429, timeout) waits kRetryMs and is asked again by a later build. UI thread only.
constexpr int64_t kRetryMs = 2 * 60'000;
std::unordered_map<std::string, std::vector<Image>> g_artistImages;   // id -> images
std::unordered_map<std::string, int64_t> g_artistRetryAt;             // id -> steadyMs() of the next attempt
std::unordered_set<std::string> g_artistLookups;                      // in flight
std::vector<std::pair<Lifetime::Ref, std::function<void()>>> g_artistWaiters;   // pages to rebuild when a batch lands

const std::vector<Image>* libraryArtistImages(const ArtistRef& a) {
    auto match = [&a](const Artist& x) { return !x.images.empty() && ((!a.id.empty() && x.id == a.id) || x.name == a.name); };
    if (auto* s = ctx().session)
        for (const auto& x : s->library().artists)
            if (match(x)) return &x.images;
    for (const auto& x : ctx().library.artists())
        if (match(x)) return &x.images;
    return nullptr;
}

std::vector<Image> artistImages(const ArtistRef& a) {
    if (const auto* lib = libraryArtistImages(a)) return *lib;
    const auto it = g_artistImages.find(a.id);
    return it != g_artistImages.end() ? it->second : std::vector<Image>{};
}

// Looks the pictures of `ids` up on a worker, one request at a time (MusicBrainz is rate limited, Spotify 429s on
// bursts): an MBID through MusicBrainz (+ Wikimedia Commons), a Spotify URI through the artist overview (only while
// logged in). The asking page is called back (`landed`, UI thread) when pictures arrive or a batch stopped early, also
// for ids another page's batch was already fetching; its rebuild shows them and asks again for what is still missing.
void fetchArtistImages(std::vector<std::string> ids, Lifetime::Ref owner, std::function<void()> landed) {
    spotify::Api* api = source::activeApi();   // captured on the UI thread
    const int64_t now = steadyMs();
    std::erase_if(ids, [&](const std::string& id) {
        if (g_artistImages.contains(id)) return true;
        if (const auto it = g_artistRetryAt.find(id); it != g_artistRetryAt.end() && now < it->second) return true;
        return !api && source::isSpotifyId(id);   // logged out / connecting: asked again after a login
    });
    if (ids.empty()) return;
    std::erase_if(g_artistWaiters, [](const auto& w) { return w.first.expired(); });
    g_artistWaiters.emplace_back(owner, std::move(landed));
    std::erase_if(ids, [](const std::string& id) { return g_artistLookups.contains(id); });   // on their way already
    if (ids.empty()) return;
    for (const auto& id : ids) g_artistLookups.insert(id);
    struct Found {
        std::string id;
        std::vector<Image> images;
        bool answered = false;
    };
    static Lifetime cacheLife;   // results land in the cache even if the page is gone by then
    async(
        Priority::Low, cacheLife.ref(),
        [ids, api, owner]() {
            std::vector<Found> found;
            for (const auto& id : ids) {
                if (owner.expired()) break;   // the page closed (and at exit the session goes away after the pages)
                try {
                    found.push_back({id, source::isSpotifyId(id) ? source::artistPage(api, id).artist.images : mb::artist(id).images, true});
                } catch (const std::exception& e) {
                    ST_LOG_DEBUG("stats", "artist picture for {}: {}", id, e.what());
                    found.push_back({id, {}, false});
                }
            }
            return found;
        },
        [ids](Result<std::vector<Found>> r) {
            for (const auto& id : ids) g_artistLookups.erase(id);   // the unfinished ones may be asked again
            bool any = false;
            if (r)
                for (auto& f : *r) {
                    if (!f.answered) {
                        g_artistRetryAt[f.id] = steadyMs() + kRetryMs;
                        continue;
                    }
                    any = any || !f.images.empty();
                    g_artistImages[f.id] = std::move(f.images);
                }
            if (!any && r && r->size() == ids.size()) return;   // complete, nothing new to show
            const auto waiters = std::move(g_artistWaiters);
            g_artistWaiters.clear();
            for (const auto& [waiter, fn] : waiters)
                if (!waiter.expired() && fn) fn();
        });
}

// Summary tile: mono label, a big value, a one-line note. The value steps down a size when a tile is narrow.
class StatTile : public ui::Widget {
public:
    static constexpr float kHeight = 128;
    StatTile(std::wstring label, std::wstring value, std::wstring note, bool highlight)
        : label_(std::move(label), type::monoLabel), value_(std::move(value), type::displayS),
          note_(std::move(note), type::caption), highlight_(highlight) {
        hitTestVisible = false;
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.bgRaised);
        const float x = r.x + 20, w = r.w - 40;
        fit(w);
        c.text(label_, {x, r.y + 20, w, 14}, highlight_ ? accent().base : col.fgTertiary);
        c.text(value_, {x - 2, r.y + 44, w + 2, 44}, col.fgPrimary, gfx::VAlign::Center);
        c.text(note_, {x, r.bottom() - 38, w, 18}, col.fgSecondary, gfx::VAlign::Center);
    }

private:
    void fit(float w) {
        if (w == fitW_) return;
        fitW_ = w;
        for (float size : {36.f, 30.f, 24.f}) {
            value_.setStyle(type::displayS.withSize(size));
            if (value_.measure().w <= w) break;
        }
    }
    gfx::Text label_, value_, note_;
    bool highlight_;
    float fitW_ = -1;
};

// Ranked song row: rank, cover, title / artist, "12 kez · 43 dk" with a small accent bar (share of the #1).
class RankRow : public ui::Widget {
public:
    RankRow(size_t rank, const Track& t, std::wstring meta, float share)
        : rank_(twoDigits(rank), type::monoDuration), title_(toWide(t.name), type::body),
          sub_(toWide(t.artistLine()), type::caption), meta_(std::move(meta), type::monoLabel), images_(t.album.images),
          share_(std::clamp(share, 0.f, 1.f)), first_(rank == 1) {
        focusable = true;
    }
    std::function<void()> onClick;
    std::function<void(gfx::Point)> onContext;
    float preferredHeight(float) override { return 64; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.overlayHover.mulAlpha(hover_));
        c.text(rank_, {r.x + 8, r.y, 28, r.h}, first_ ? accent().base : col.fgTertiary, gfx::VAlign::Center);
        const Rect art{r.x + 44, r.cy() - 22, 44, 44};
        drawArtwork(c, images_, art, gfx::metrics::radiusSurface, Placeholder::Album);
        const float mw = std::max(kBarW, std::ceil(meta_.measure().w));
        const float tx = art.right() + 14, tw = r.right() - tx - mw - 28;
        c.text(title_, {tx, r.cy() - 19, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
        c.text(sub_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
        const float mx = r.right() - 8 - mw;
        c.text(meta_, {mx, r.cy() - 14, mw, 14}, col.fgTertiary, gfx::VAlign::Center);
        const Rect bar{r.right() - 8 - kBarW, r.cy() + 7, kBarW, 2};
        c.fillRect(bar, col.hairSubtle);
        c.fillRect({bar.x, bar.y, std::max(2.f, kBarW * share_), bar.h}, accent().base);
        c.hline(tx, r.right(), r.bottom() - 1, col.hairSubtle);
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            if (onContext) onContext(e.windowPos);
            return false;
        }
        return e.button == ui::MouseButton::Left;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (!rect().contains(e.pos) || !onClick) return;
        const auto click = onClick;
        click();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable): Enter / Space play, menu key / Shift+F10 open the track menu.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        const auto click = onClick;   // playing may rebuild this page
        click();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if ((e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) || !onContext) return false;
        const Rect r = toWindow(rect());
        const auto context = onContext;
        context({r.x + 96, r.bottom()});
        return true;
    }

private:
    static constexpr float kBarW = 96;
    gfx::Text rank_, title_, sub_, meta_;
    std::vector<Image> images_;
    float share_;
    bool first_;
    ui::Anim hover_;
};

// Fixed-height rows in one column, or two when wide. Column-major, so a ranking reads down the first column.
class RowColumns : public ui::Widget {
public:
    static constexpr float kRowH = 64, kGap = 48, kTwoColumnsMin = 880;
    RowColumns() { hitTestVisible = false; }
    float preferredHeight(float w) override { return rows(w) * kRowH; }
    void layout() override {
        const float w = rect().w;
        const bool two = w >= kTwoColumnsMin;
        const float cw = two ? std::floor((w - kGap) * 0.5f) : w;
        const int n = rows(w);
        int i = 0;
        for (auto& child : children()) {
            const int col = n > 0 ? i / n : 0, row = n > 0 ? i % n : 0;
            child->setRect({col * (cw + kGap), row * kRowH, cw, kRowH});
            ++i;
        }
    }

private:
    int rows(float w) const {
        const int n = static_cast<int>(children().size());
        return w >= kTwoColumnsMin ? (n + 1) / 2 : n;
    }
};

class StatsPage : public ScrollPage {
public:
    StatsPage() {
        store().subscribe(life_.ref(), [this] { scheduleRebuild(); });
        // Logging in / out changes which artist / album ids can open their page (and which photos can be looked up).
        if (ctx().session)
            ctx().session->subscribe(life_.ref(), [this] {
                if (source::loggedIn() != builtLoggedIn_) scheduleRebuild();
            });
        rebuild();
    }

    // Back navigation: Router restores the offset right after the constructor. Once the history is loaded this page is
    // built synchronously, so apply it now; otherwise ScrollPage keeps it pending until the next (unrelated) rebuild.
    void restoreScroll(float y) override {
        ScrollPage::restoreScroll(y);
        if (y > 0 && built_) contentReady();
    }

private:
    void scheduleRebuild() {
        // Coalesced, and never inside the click / key handler of a widget the rebuild destroys.
        if (rebuildQueued_) return;
        rebuildQueued_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            if (ref.expired()) return;
            rebuildQueued_ = false;
            rebuild();
        });
    }

    void rebuild() {
        auto& s = store();
        if (!s.loaded()) {   // the history is still being read (the store notifies when it lands)
            showSkeleton(3);
            return;
        }
        built_ = true;
        builtLoggedIn_ = source::loggedIn();
        const int64_t now = nowUnix();
        const StatsSummary sum = s.summarize(g_period, now, 12, 20);
        const bool anything = s.playCount() > 0 || s.currentListenedMs() >= listen::kMinPlayMs;
        auto* c = resetContent();

        // --- Header: title (+ "Geçmişi temizle"), what counts, period selector + date range.
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"İstatistikler"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        Button* clearBtn = anything ? top->add<Button>(ButtonKind::Ghost, tr(L"Geçmişi temizle"), "trash") : nullptr;
        if (clearBtn) clearBtn->onClick = [] { confirmClear(); };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, clearBtn](ui::Box& b) {
            const float w = b.rect().w;
            title->setRect({0, 0, w * 0.6f, 64});
            if (clearBtn) {
                const float bw = clearBtn->naturalWidth();
                clearBtn->setRect({w - bw, 12, bw, 40});
            }
        };
        auto* note = c->add<ui::Label>(tr(L"Bu bilgisayarda çaldıkların · 30 saniyeyi geçen her çalma bir dinleme "
                                          L"sayılır"),
                                       type::secondary, ui::Tone::Tertiary);
        c->setSpacingBefore(note, 4);
        if (!anything) {
            c->add<MessagePanel>("stats", tr(L"Henüz dinleme geçmişi yok"),
                                 tr(L"Şarkı dinledikçe en çok dinlediklerin, toplam süren ve son çalınanlar burada "
                                    L"birikir."));
            contentReady();
            return;
        }

        auto* bar = c->add<ui::Box>();
        c->setSpacingBefore(bar, 20);
        std::vector<std::wstring> periods{tr(L"Son 7 gün"), tr(L"Son 30 gün"), tr(L"Tüm zamanlar")};
        auto* seg = bar->add<Segmented>(std::move(periods), static_cast<int>(g_period));
        seg->onChange = [this](int i) {
            g_period = static_cast<StatsPeriod>(i);
            scheduleRebuild();
        };
        auto* range = bar->add<ui::Label>(rangeLabel(sum, now), type::monoLabel, ui::Tone::Tertiary);
        range->setAlign(gfx::TextAlign::Trailing);
        bar->onPreferredHeight = [](float) { return 36.f; };
        bar->onLayout = [seg, range](ui::Box& b) {
            const float w = b.rect().w, sw = seg->naturalWidth();
            seg->setRect({0, 2, sw, 32});
            range->setRect({sw + 24, 2, std::max(0.f, w - sw - 24), 32});
        };

        if (sum.listenedMs <= 0) {
            c->add<MessagePanel>("stats", tr(L"Bu dönemde dinleme yok"),
                                 g_period == StatsPeriod::Week
                                     ? tr(L"Son 7 günde kaydedilmiş bir dinleme yok. Daha uzun bir döneme bak.")
                                     : tr(L"Son 30 günde kaydedilmiş bir dinleme yok. Tüm zamanlara bak."));
            contentReady();
            return;
        }

        // --- Summary tiles.
        c->setSpacingBefore(addTiles(c, sum, now), 24);
        if (sum.streams == 0) {
            c->add<MessagePanel>("stats", tr(L"Henüz sayılan bir dinleme yok"),
                                 tr(L"Bir şarkı 30 saniyeyi geçince (60 saniyeden kısa şarkılarda yarısını) bir "
                                    L"dinleme sayılır."));
            contentReady();
            return;
        }

        // --- En çok dinlenen şarkılar (click plays the list from there).
        std::vector<Track> topTracks;
        for (size_t i = 0; i < sum.topTracks.size() && i < 10; ++i) topTracks.push_back(sum.topTracks[i].track);
        c->add<SectionHeader>(tr(L"En çok dinlenen şarkılar"), twoDigits(topTracks.size()));
        auto* rows = c->add<RowColumns>();
        c->setSpacingBefore(rows, 12);
        const int best = sum.topTracks.empty() ? 1 : std::max(1, sum.topTracks[0].streams);
        for (size_t i = 0; i < topTracks.size(); ++i) {
            const auto& st = sum.topTracks[i];
            const std::wstring meta = i18n::plural(L"{} kez", st.streams) + L" · " + durationLabel(st.listenedMs);
            auto* row = rows->add<RankRow>(i + 1, st.track, meta, static_cast<float>(st.streams) / static_cast<float>(best));
            row->onClick = [topTracks, i] {
                ctx().player->playContext(topTracks, static_cast<int>(i), {"stats", tr(L"En çok dinlenen şarkılar")});
            };
            row->onContext = [t = topTracks[i]](gfx::Point wp) { showTrackMenu({t}, wp); };
        }

        // --- En çok dinlenen sanatçılar (round cards -> artist page, or a search when the credit has no id).
        if (!sum.topArtists.empty()) {
            c->add<SectionHeader>(tr(L"En çok dinlenen sanatçılar"), twoDigits(sum.topArtists.size()));
            auto* grid = addCardRow(c, 150, 0);   // wraps: every counted card is visible (and looked up)
            c->setSpacingBefore(grid, 16);
            std::vector<std::string> lookup;
            for (const auto& a : sum.topArtists) {
                if (routable(a.artist.id, "spotify:artist:") && !libraryArtistImages(a.artist)) lookup.push_back(a.artist.id);
                const std::wstring sub = i18n::plural(L"{} dinleme", a.streams) + L" · " + durationLabel(a.listenedMs);
                auto* card = grid->add<MediaCard>(toWide(a.artist.name), sub, artistImages(a.artist), MediaCard::Shape::Circle,
                                                  Placeholder::Artist);
                const Route open = routable(a.artist.id, "spotify:artist:") ? Route{RouteKind::Artist, a.artist.id}
                                                                           : Route{RouteKind::Search, a.artist.name};
                card->onOpen = [open] { ctx().router->navigate(open); };
            }
            fetchArtistImages(std::move(lookup), life_.ref(), [this] { scheduleRebuild(); });
        }

        // --- En çok dinlenen albümler.
        if (!sum.topAlbums.empty()) {
            c->add<SectionHeader>(tr(L"En çok dinlenen albümler"), twoDigits(sum.topAlbums.size()));
            auto* grid = addCardRow(c, 150, 0);
            c->setSpacingBefore(grid, 16);
            for (const auto& a : sum.topAlbums) {
                const std::wstring by = a.variousArtists ? std::wstring(tr(L"Çeşitli sanatçılar")) : toWide(a.artist);
                std::wstring sub = i18n::plural(L"{} dinleme", a.streams);
                if (!by.empty()) sub = by + L" · " + sub;
                auto* card = grid->add<MediaCard>(toWide(a.album.name), sub, a.album.images);
                if (routable(a.album.id, "spotify:album:")) {
                    const std::string id = a.album.id;
                    card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                    card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
                } else {
                    const std::string q = a.artist.empty() || a.variousArtists ? a.album.name : a.artist + " " + a.album.name;
                    card->onOpen = [q] { ctx().router->navigate({RouteKind::Search, q}); };
                }
            }
        }

        // --- Son çalınanlar.
        if (!sum.recent.empty()) {
            std::vector<Track> recent;
            for (const auto& r : sum.recent) recent.push_back(r.track);
            c->add<SectionHeader>(tr(L"Son çalınanlar"), twoDigits(recent.size()));
            auto* list = c->add<RowColumns>();
            c->setSpacingBefore(list, 12);
            for (size_t i = 0; i < recent.size(); ++i) {
                const auto& t = recent[i];
                auto* row = list->add<ListRow>(toWide(t.name), toWide(t.artistLine()), relativeTime(sum.recent[i].startedAt), t.album.images);
                row->onClick = [recent, i] {
                    ctx().player->playContext(recent, static_cast<int>(i), {"stats", tr(L"Son çalınanlar")});
                };
                row->onContext = [t](gfx::Point wp) { showTrackMenu({t}, wp); };
            }
        }
        contentReady();
    }

    static std::wstring rangeLabel(const StatsSummary& sum, int64_t now) {
        // The rolling 7 x 24 h / 30 x 24 h window summarize() counted (it starts at this time on the first date).
        if (g_period != StatsPeriod::All) return trDate(sum.periodStart) + L" – " + trDate(now);
        if (sum.periodStart <= 0) return {};
        return i18n::format(tr(L"İlk kayıt {}"), {trDate(sum.periodStart, true)});
    }

    // Four tiles in a row, 2 x 2 when narrow. Returns the container.
    static ui::Widget* addTiles(ui::Column* c, const StatsSummary& sum, int64_t now) {
        // Days covered: the period, or less when the history is younger than it (the first play ever).
        const auto& plays = store().plays();
        const int64_t first = std::max(sum.periodStart, plays.empty() ? now : plays.front().startedAt);
        const double days = std::max(1.0, std::ceil(static_cast<double>(now - first) / 86400.0));
        const std::wstring perDay = durationLabel(static_cast<int64_t>(sum.listenedMs / days));
        const std::wstring perTrack = perItem(sum.streams, sum.distinctTracks);
        const std::wstring perArtist = perItem(sum.streams, sum.distinctArtists);
        auto* box = c->add<ui::Box>();
        std::vector<ui::Widget*> tiles{
            box->add<StatTile>(tr(L"Toplam dinleme"), durationLabel(sum.listenedMs),
                               i18n::format(tr(L"Günde ortalama {}"), {perDay}), true),
            box->add<StatTile>(tr(L"Dinleme sayısı"), thousands(sum.streams), tr(L"30 saniyeyi geçen çalmalar"), false),
            box->add<StatTile>(tr(L"Farklı şarkı"), thousands(sum.distinctTracks),
                               i18n::format(tr(L"Şarkı başına {} dinleme"), {perTrack}), false),
            box->add<StatTile>(tr(L"Farklı sanatçı"), thousands(sum.distinctArtists),
                               i18n::format(tr(L"Sanatçı başına {} dinleme"), {perArtist}), false),
        };
        constexpr float gap = gfx::metrics::gridGap, minTile = 200;
        auto columns = [](float w) { return w >= 4 * minTile + 3 * gap ? 4 : 2; };
        box->onPreferredHeight = [columns](float w) {
            return columns(w) == 4 ? StatTile::kHeight : StatTile::kHeight * 2 + gap;
        };
        box->onLayout = [tiles, columns](ui::Box& b) {
            const int cols = columns(b.rect().w);
            const float tw = (b.rect().w - gap * (cols - 1)) / cols;
            for (size_t i = 0; i < tiles.size(); ++i) {
                const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
                tiles[i]->setRect({col * (tw + gap), row * (StatTile::kHeight + gap), tw, StatTile::kHeight});
            }
        };
        return box;
    }

    static void confirmClear() {
        ui::Dialog::confirm(
            ctx().window, tr(L"Dinleme geçmişi silinsin mi?"),
            tr(L"İstatistiklerdeki tüm dinlemeler bu bilgisayardan kalıcı olarak silinecek. Bu işlem geri alınamaz."),
            tr(L"Temizle"),
            [] {
                auto& s = store();
                s.clear(nowUnix());
                s.saveAsync();
                g_lastSaveMs = steadyMs();
                toast(tr(L"Dinleme geçmişi temizlendi"));
            },
            true);
    }

    bool rebuildQueued_ = false;
    bool built_ = false;           // real content (not the loading skeleton) is on screen
    bool builtLoggedIn_ = false;
};

} // namespace

ListenStats& listenStats() { return store(); }

void initListenStats() {
    auto& c = ctx();
    // The history is parsed on a worker (a big file takes a moment); plays recorded meanwhile are merged on top.
    static Lifetime life;   // the store lives as long as the process
    async(
        Priority::Normal, life.ref(), [file = store().file()] { return ListenStats::readFile(file); },
        [](Result<ListenStats::Snapshot> r) {
            if (!r) ST_LOG_WARN("stats", "reading listening.json failed: {}", r.errorMessage());
            store().adopt(r ? std::move(*r) : ListenStats::Snapshot{});
        });
    // A new track: store the previous play, start the next (or continue it: re-resolve / "Yanlış eşleşme?").
    // A radio station is not recorded: hours of a station would count as one very long "song".
    c.trackChangedHooks.push_back([](const catalog::Track& t) {
        if (radio::isStationId(t.id)) {
            store().stop(steadyMs());
            return;
        }
        const auto* p = ctx().player;
        store().trackStarted(t, p ? p->positionMs() : 0, nowUnix(), steadyMs());
    });
    // Pause / resume / seek / stop: sample right away, so a seek or a pause splits the listened time exactly.
    c.playerChangedHooks.push_back([] { sample(); });
    // Every ~2 s: accumulate the listened time; save when something was stored, at most every 30 s. The UI thread only
    // snapshots the plays; serializing and writing the file (flushed, the previous one kept) runs on a worker.
    c.housekeepingHooks.push_back([] {
        sample();
        auto& s = store();
        if (s.dirty() && s.loaded() && steadyMs() - g_lastSaveMs >= 30'000) {
            g_lastSaveMs = steadyMs();
            s.saveAsync();
        }
    });
    // Exit: the current play is written as it stands (it keeps counting if the app goes on, e.g. a cancelled logoff).
    // Synchronous; a worker save still running is waited for, and an older one landing later is skipped.
    c.persistHooks.push_back([] {
        sample();
        syncSampleTimer(false);   // the next sample turns it back on if playback goes on
        auto& s = store();
        if (s.loaded() && (s.dirty() || s.currentListenedMs() >= listen::kMinPlayMs)) s.save();
    });
}

std::unique_ptr<Page> makeStatsPage() { return std::make_unique<StatsPage>(); }

} // namespace st::app
