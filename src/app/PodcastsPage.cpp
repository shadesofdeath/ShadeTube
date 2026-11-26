// Podcastler: podcasts from Apple's directory and RSS feeds (app/Podcasts) — the pages, the episode widgets, the pieces
// other screens use (PodcastUi.h) and the wiring: the player's media lookup and resume position, progress / played
// tracking, downloads and the refresh of subscriptions.
//
// Routes (Route{RouteKind::Podcasts, id}): "" = the Podcastler page; "search:<text>" = directory results;
// "feed:<feed URL>" = a show; "apple:<directory id>" = a show from the directory (its feed looked up first);
// "new" = new episodes of the subscriptions; "downloads" = downloaded episodes; "spotify:show:<id>" = a Spotify show
// (its public feed, else why it can't play); "spotify" = the shows followed on Spotify, to import (app/PodcastsSpotify).
#include "app/PodcastUi.h"

#include "app/CollapsingHeader.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Podcasts.h"
#include "app/PodcastsSpotify.h"
#include "app/Source.h"
#include "app/Updater.h"
#include "player/Player.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using podcast::DirectoryEntry;
using podcast::Episode;
using podcast::Show;
using ui::Button;
using ui::ButtonKind;
using CancelSource = YoutubeExplode::CancellationTokenSource;
namespace fs = std::filesystem;
namespace type = gfx::type;

namespace {

// ===================================================================================================
// Helpers

void repaint() {
    if (auto* w = ctx().window) w->invalidate();
}

// The directory's store for charts and search: Windows' home location, else the app region.
std::string homeCountry() { return podcastDirectoryCountry(); }

fs::path downloadsRoot() {
    const auto& s = Settings::get();
    return s.downloadsDir.empty() ? paths::downloadsDir() : fs::path(toWide(s.downloadsDir));
}

// A feed the user added by its URL may live on the local network, and so may its audio.
bool allowLocal(const std::string& feedUrl) {
    const auto* s = podcast::store().subscription(feedUrl);
    return s && s->manual;
}

int yearOf(int64_t unix) {
    tm t{};
    const __time64_t tt = unix;
    if (_localtime64_s(&t, &tt) != 0) return 0;
    return t.tm_year + 1900;
}

// "12 EYL" this year, "12 EYL 2023" before.
std::wstring dateLabel(int64_t published) {
    if (published <= 0) return {};
    return trDate(published, yearOf(published) != yearOf(nowUnix()));
}

// "12 EYL · 58 DK · S2 B14"
std::wstring metaLine(const Episode& e) {
    std::wstring s = dateLabel(e.published);
    auto add = [&](const std::wstring& part) {
        if (part.empty()) return;
        if (!s.empty()) s += L" · ";
        s += part;
    };
    if (e.durationMs > 0) add(totalDuration(e.durationMs));
    if (e.season > 0 && e.number > 0) add(i18n::format(tr(L"S{} B{}"), {std::to_wstring(e.season), std::to_wstring(e.number)}));
    else if (e.number > 0) add(i18n::format(tr(L"Bölüm {}"), {std::to_wstring(e.number)}));
    return toUpperTr(s);
}

// At most `max` characters of `utf8` on one line (row and header previews of the show notes).
std::wstring snippet(const std::string& utf8, size_t max) {
    std::wstring w = toWide(std::string_view(utf8).substr(0, max * 4));
    if (w.size() > max) w.resize(max);
    for (auto& ch : w)
        if (ch == L'\n') ch = L' ';
    return w;
}

bool isCurrent(const std::string& id) {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    return t && t->id == id;
}

bool isPlayingNow(const std::string& id) { return isCurrent(id) && ctx().player->isPlaying(); }

// ===================================================================================================
// Playback

player::PlayContext contextFor(const Episode& e) { return {"podcast:" + e.feedUrl, toWide(e.show)}; }

void playEpisodes(const std::vector<Episode>& list, size_t index) {
    if (list.empty() || !ctx().player) return;
    index = std::min(index, list.size() - 1);
    podcast::store().rememberQueue(list, index);
    ctx().player->playContext(podcast::toTracks(list), static_cast<int>(index), contextFor(list[index]));
}

void playEpisode(const Episode& e) { playEpisodes({e}, 0); }

void togglePlay(const Episode& e) {
    if (isCurrent(e.id())) ctx().player->togglePause();
    else playEpisode(e);
}

// The episodes waiting in the player's queue are kept with the store's queue, so a restored session still finds them.
void syncQueueEpisodes() {
    const auto* p = ctx().player;
    if (!p) return;
    std::vector<Episode> list;
    size_t playing = 0;
    const auto& order = p->order();
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        const auto& t = p->items()[order[i]];
        if (!catalog::isPodcastId(t.id)) continue;
        if (const Episode* e = podcast::store().find(t.id)) {
            if (i == p->currentOrderIndex()) playing = list.size();
            list.push_back(*e);
        }
    }
    if (!list.empty()) podcast::store().rememberQueue(list, playing);
}

// ===================================================================================================
// Downloads (one at a time, in the background)

struct DownloadQueue {
    std::deque<Episode> waiting;
    std::string active;   // episode id
    float progress = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    bool userCanceled = false;
    bool quitting = false;     // the app is exiting: the active download stops and keeps its .part
};

DownloadQueue& dlq() {
    static DownloadQueue q;
    return q;
}

// nullopt = neither waiting nor downloading; else 0..1.
std::optional<float> downloadProgress(const std::string& id) {
    const auto& q = dlq();
    if (q.active == id) return q.progress;
    for (const auto& e : q.waiting)
        if (e.id() == id) return 0.f;
    return std::nullopt;
}

bool isDownloaded(const std::string& id) { return !podcast::store().downloadedFile(id).empty(); }

void pumpDownloads();

void startDownload(const Episode& e) {
    if (isDownloaded(e.id()) || downloadProgress(e.id())) return;
    podcast::store().remember(e);
    dlq().waiting.push_back(e);
    toast(tr(L"Bölüm indiriliyor"));
    pumpDownloads();
    repaint();
}

void cancelDownload(const std::string& id) {
    auto& q = dlq();
    std::erase_if(q.waiting, [&](const Episode& e) { return e.id() == id; });
    if (q.active == id && q.cancel) {
        q.userCanceled = true;
        q.cancel->store(true);
    }
    repaint();
}

void deleteDownload(const Episode& e) {
    const std::string file = podcast::store().downloadedFile(e.id());
    if (file.empty()) return;
    std::error_code ec;
    fs::remove(fs::path(toWide(file)), ec);
    // The show's folder goes too once it is empty (never anything else).
    const fs::path dir = fs::path(toWide(file)).parent_path();
    if (fs::is_empty(dir, ec) && !ec) fs::remove(dir, ec);
    podcast::store().clearDownload(e.id());
    toast(tr(L"İndirilen bölüm silindi"));
}

void pumpDownloads() {
    auto& q = dlq();
    if (!q.active.empty() || q.waiting.empty()) return;
    const Episode e = q.waiting.front();
    q.waiting.pop_front();
    q.active = e.id();
    q.progress = 0;
    q.userCanceled = false;
    q.cancel = std::make_shared<std::atomic<bool>>(false);
    const fs::path target = podcast::downloadPath(downloadsRoot(), e);
    const bool local = allowLocal(e.feedUrl);
    auto cancel = q.cancel;
    const std::string id = q.active;
    static Lifetime life;   // the queue lives as long as the process
    ST_LOG_INFO("podcasts", "download: {}", e.title);
    async(
        Priority::Low, life.ref(),
        [e, target, local, cancel, id] {
            auto last = std::chrono::steady_clock::now();
            return podcast::Client::shared().download(
                e.url, target, local,
                [id, &last](int64_t done, int64_t total) {
                    if (total <= 0) return;
                    const auto now = std::chrono::steady_clock::now();
                    if (done < total && now - last < std::chrono::milliseconds(200)) return;
                    last = now;
                    const float f = static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
                    Dispatcher::post([id, f] {
                        if (auto& dq = dlq(); dq.active == id) {
                            dq.progress = f;
                            repaint();
                        }
                    });
                },
                *cancel);
        },
        [e, target](Result<int64_t> r) {
            auto& dq = dlq();
            const bool canceled = dq.userCanceled;
            dq.active.clear();
            dq.cancel.reset();
            if (dq.quitting) return;
            if (r) {
                podcast::store().setDownloaded(e, toUtf8(target.wstring()), *r, nowUnix());
                toast(i18n::format(tr(L"\"{}\" indirildi"), {toWide(e.title)}));
            } else if (canceled) {
                std::error_code ec;
                fs::path part = target;
                part += L".part";
                fs::remove(part, ec);
            } else {
                ST_LOG_WARN("podcasts", "download of \"{}\" failed: {}", e.title, r.errorMessage());
                toast(i18n::format(tr(L"\"{}\" indirilemedi"), {toWide(e.title)}), true);
            }
            repaint();
            pumpDownloads();
        });
}

// ===================================================================================================
// Details and menus

void showEpisodeDetails(const Episode& e) {
    std::wstring sub = toWide(e.show);
    if (const std::wstring meta = metaLine(e); !meta.empty()) sub += L" · " + meta;
    auto* d = ui::Dialog::open(ctx().window, toWide(e.title), sub, 640);
    if (!d) return;
    auto* box = d->body()->add<ui::Box>();
    auto* scroll = box->add<ui::ScrollView>();
    auto* col = scroll->setContent<ui::Column>(0.f);
    auto* text = col->add<ui::Label>(e.description.empty() ? tr(L"Bu bölüm için not yok.") : toWide(e.description), type::secondary,
                                     ui::Tone::Secondary);
    text->setWrap(true);
    text->setVAlign(gfx::VAlign::Top);
    box->onPreferredHeight = [](float) { return 320.f; };
    box->onLayout = [scroll](ui::Box& b) { scroll->setRect({0, 0, b.rect().w, b.rect().h}); };
    d->addButton(tr(L"Kapat"), ButtonKind::Ghost, {});
    if (isDownloaded(e.id())) d->addButton(tr(L"İndirileni sil"), ButtonKind::Secondary, [e] { deleteDownload(e); });
    else if (!downloadProgress(e.id())) d->addButton(tr(L"İndir"), ButtonKind::Secondary, [e] { startDownload(e); });
    d->addButton(isPlayingNow(e.id()) ? tr(L"Duraklat") : tr(L"Çal"), ButtonKind::Primary, [e] { togglePlay(e); });
}

void openShow(const std::string& feedUrl) { ctx().router->navigate({RouteKind::Podcasts, "feed:" + feedUrl}); }

std::vector<ui::MenuItem> episodeMenuItems(const Episode& e, bool withShow) {
    std::vector<ui::MenuItem> items;
    const std::string id = e.id();
    const bool playing = isPlayingNow(id);
    items.push_back({playing ? tr(L"Duraklat") : tr(L"Çal"), playing ? "pause" : "play", L"", [e] { togglePlay(e); }});
    items.push_back({tr(L"Sonra çal"), "queue", L"", [e] {
                         podcast::store().remember(e);
                         ctx().player->playNext(podcast::toTrack(e));
                         toast(tr(L"Sıradaki olarak eklendi"));
                     }});
    items.push_back({tr(L"Sıraya ekle"), "plus", L"", [e] {
                         podcast::store().remember(e);
                         ctx().player->enqueue({podcast::toTrack(e)});
                         toast(tr(L"Sıraya eklendi"));
                     }});
    items.push_back(ui::MenuItem::sep());
    if (isDownloaded(id)) {
        ui::MenuItem del{tr(L"İndirileni sil"), "trash", L"", [e] { deleteDownload(e); }};
        del.destructive = true;
        items.push_back(std::move(del));
    } else if (downloadProgress(id)) {
        items.push_back({tr(L"İndirmeyi iptal et"), "close", L"", [id] { cancelDownload(id); }});
    } else {
        items.push_back({tr(L"İndir"), "download", L"", [e] { startDownload(e); }});
    }
    const bool played = podcast::store().isPlayed(id);
    items.push_back({played ? tr(L"Oynatılmadı olarak işaretle") : tr(L"Oynatıldı olarak işaretle"), played ? "refresh" : "check", L"",
                     [e, played] { podcast::store().setPlayed(e, !played, nowUnix()); }});
    items.push_back({tr(L"Bölüm notları"), "info", L"", [e] { showEpisodeDetails(e); }});
    if (withShow) items.push_back({tr(L"Podcast'e git"), "arrow-up-right", L"", [feed = e.feedUrl] { openShow(feed); }});
    items.push_back({tr(L"Ses adresini kopyala"), "link", L"", [url = e.url] {
                         copyText(toWide(url));
                         toast(tr(L"Adres kopyalandı"));
                     }});
    return items;
}

// ===================================================================================================
// Episode list: virtualized rows painted from the model (no per-row widgets). Click opens the show notes, the round
// button plays / pauses, the arrow downloads; right click / the menu key open the episode menu; Up / Down / Home /
// End / PageUp / PageDown move the keyboard cursor and Enter plays.

constexpr float kEpRowH = 104;

// Downloaded: an accent disc with a check (icons are one-color masks, so the "downloaded" glyph's own check is lost).
void drawDownloadedMark(Canvas& c, gfx::Point center, float radius) {
    c.fillCircle(center, radius, accent().base);
    const float s = std::round(radius * 1.3f);
    c.icon("check", {center.x - s * 0.5f, center.y - s * 0.5f, s, s}, accent().onAccent);
}

class EpisodeList : public ui::Widget {
public:
    struct Options {
        bool art = false;    // episode artwork on the left (lists that mix shows)
        bool show = false;   // the show's name in the meta line
    };
    explicit EpisodeList(Options opts) : opts_(opts) { focusable = true; }

    void setEpisodes(std::vector<Episode> list) {
        episodes_ = std::move(list);
        texts_.clear();
        cursor_ = std::clamp(cursor_, 0, std::max(0, static_cast<int>(episodes_.size()) - 1));
        podcast::store().remember(episodes_);
        requestLayout();
        invalidate();
    }
    const std::vector<Episode>& episodes() const { return episodes_; }

    float preferredHeight(float) override { return static_cast<float>(episodes_.size()) * kEpRowH; }
    LPCWSTR cursor() const override { return hoverRow_ >= 0 ? IDC_HAND : IDC_ARROW; }
    std::wstring tooltip() const override {
        if (hoverRow_ < 0 || hoverRow_ >= static_cast<int>(episodes_.size())) return {};
        const auto& e = episodes_[hoverRow_];
        switch (hoverPart_) {
        case Part::Play: return isPlayingNow(e.id()) ? tr(L"Duraklat") : tr(L"Çal");
        case Part::Download:
            if (isDownloaded(e.id())) return tr(L"İndirildi");
            if (auto p = downloadProgress(e.id())) return i18n::format(tr(L"İndiriliyor… %{}"), {std::to_wstring(static_cast<int>(*p * 100))});
            return tr(L"İndir");
        case Part::More: return tr(L"Diğer seçenekler");
        default: return {};
        }
    }

    void paint(Canvas& c) override {
        const Rect vis = c.clip();
        const Rect r = rect();
        const int n = static_cast<int>(episodes_.size());
        const int first = std::max(0, static_cast<int>((vis.y - r.y) / kEpRowH));
        const int last = std::min(n - 1, static_cast<int>((vis.bottom() - r.y) / kEpRowH));
        bool live = false, loading = false;
        for (int i = first; i <= last; ++i) paintRow(c, i, live, loading);
        // The playing episode's progress and running downloads move: repaint while they are on screen.
        if (auto* w = window()) {
            if (loading) w->invalidateAfter(250);
            else if (live) w->invalidateAfter(1000);
        }
    }

    void onMouseMove(const ui::MouseEvent& e) override {
        const int row = rowAt(e.pos.y);
        const Part part = row >= 0 ? partAt(row, e.pos) : Part::None;
        if (row != hoverRow_ || part != hoverPart_) {
            hoverRow_ = row;
            hoverFade_.set(row);
            hoverPart_ = part;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hoverRow_ = -1;
        hoverFade_.set(-1);
        hoverPart_ = Part::None;
        invalidate();
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        const int row = rowAt(e.pos.y);
        if (row < 0) return false;
        cursor_ = row;
        const Episode ep = episodes_[row];   // the actions below may rebuild this list
        if (e.button == ui::MouseButton::Right) {
            ui::Menu::open(ctx().window, e.windowPos, episodeMenuItems(ep, opts_.show));
            invalidate();
            return false;
        }
        if (e.button != ui::MouseButton::Left) return false;
        switch (partAt(row, e.pos)) {
        case Part::Play: togglePlay(ep); break;
        case Part::Download:
            if (isDownloaded(ep.id())) confirmDelete(ep);
            else if (downloadProgress(ep.id())) cancelDownload(ep.id());
            else startDownload(ep);
            break;
        case Part::More: {
            const Rect b = toWindow(moreRect(rowRect(row)));
            ui::Menu::open(ctx().window, {b.x, b.bottom()}, episodeMenuItems(ep, opts_.show));
            break;
        }
        default: showEpisodeDetails(ep); break;
        }
        invalidate();
        return true;
    }

    Rect focusRect() const override {
        if (episodes_.empty()) return rect();
        return rowRect(std::clamp(cursor_, 0, static_cast<int>(episodes_.size()) - 1));
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        const int n = static_cast<int>(episodes_.size());
        if (n == 0) return false;
        int cur = std::clamp(cursor_, 0, n - 1);
        switch (e.vk) {
        case VK_DOWN: cur = std::min(n - 1, cur + 1); break;
        case VK_UP: cur = std::max(0, cur - 1); break;
        case VK_NEXT: cur = std::min(n - 1, cur + 6); break;
        case VK_PRIOR: cur = std::max(0, cur - 6); break;
        case VK_HOME: cur = 0; break;
        case VK_END: cur = n - 1; break;
        case VK_RETURN: {
            const Episode ep = episodes_[cur];
            togglePlay(ep);
            return true;
        }
        case VK_APPS:
        case VK_F10: {
            if (e.vk == VK_F10 && !e.shift) return false;
            const Rect rr = toWindow(rowRect(cur));
            ui::Menu::open(ctx().window, {rr.x + 48, rr.bottom()}, episodeMenuItems(episodes_[cur], opts_.show));
            return true;
        }
        default: return false;
        }
        cursor_ = cur;
        invalidate();
        return true;
    }

private:
    enum class Part { None, Row, Play, Download, More };
    struct RowText {
        gfx::Text meta, title, desc;
    };

    Rect rowRect(int i) const { return {rect().x, rect().y + static_cast<float>(i) * kEpRowH, rect().w, kEpRowH}; }
    int rowAt(float y) const {
        if (y < rect().y) return -1;
        const int i = static_cast<int>((y - rect().y) / kEpRowH);
        return i < static_cast<int>(episodes_.size()) ? i : -1;
    }
    Rect moreRect(const Rect& row) const { return {row.right() - 12 - 32, row.cy() - 16, 32, 32}; }
    Rect downloadRect(const Rect& row) const { return {row.right() - 12 - 32 - 8 - 32, row.cy() - 16, 32, 32}; }
    Rect playRect(const Rect& row) const { return {row.right() - 12 - 32 - 8 - 32 - 12 - 40, row.cy() - 20, 40, 40}; }
    Part partAt(int row, gfx::Point p) const {
        const Rect rr = rowRect(row);
        if (playRect(rr).contains(p)) return Part::Play;
        if (downloadRect(rr).contains(p)) return Part::Download;
        if (moreRect(rr).contains(p)) return Part::More;
        return Part::Row;
    }

    RowText& text(int i) {
        if (auto it = texts_.find(i); it != texts_.end()) return it->second;
        if (texts_.size() > 80) texts_.clear();
        const auto& e = episodes_[i];
        gfx::TextOptions two;
        two.wrap = true;
        two.maxLines = 2;
        std::wstring meta = metaLine(e);
        if (opts_.show && !e.show.empty()) meta = toUpperTr(toWide(e.show)) + (meta.empty() ? L"" : L" · " + meta);
        // Notes as one flowing text: line breaks would waste the two lines.
        const std::wstring desc = snippet(e.description, 400);
        return texts_.emplace(i, RowText{gfx::Text(meta, type::monoMeta), gfx::Text(toWide(e.title), type::body), gfx::Text(desc, type::secondary, two)})
            .first->second;
    }

    void confirmDelete(const Episode& ep) {
        ui::Dialog::confirm(ctx().window, tr(L"İndirilen bölüm silinsin mi?"), toWide(ep.title), tr(L"Sil"), [ep] { deleteDownload(ep); }, true);
    }

    void paintRow(Canvas& c, int i, bool& live, bool& loading) {
        const auto& col = colors();
        const auto& acc = accent();
        const Episode& e = episodes_[i];
        const std::string id = e.id();
        const Rect r = rowRect(i);
        const bool current = isCurrent(id);
        const bool playing = current && ctx().player->isPlaying();
        const bool hot = i == hoverRow_;
        const auto* st = podcast::store().state(id);
        const bool played = st && st->played;
        if (current) c.fillRounded(r, gfx::radii().surface, acc.tint06);
        if (const float hv = hoverFade_.of(i); hv > 0.001f) c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hv));
        float x = r.x + 12;
        if (opts_.art) {
            const Rect art{x, r.cy() - 36, 72, 72};
            std::vector<catalog::Image> images;
            if (!e.image.empty()) images.push_back({e.image, 0, 0});
            drawArtwork(c, images, art, gfx::radii().art, Placeholder::Album, false, Priority::Normal);
            x = art.right() + 16;
        }
        const Rect play = playRect(r), dl = downloadRect(r), more = moreRect(r);
        const float tw = std::max(0.f, play.x - 20 - x);
        auto& t = text(i);

        // Meta line: date · length (· show), then the state: downloaded, played or the time left.
        float mx = x;
        if (isDownloaded(id)) {
            drawDownloadedMark(c, {mx + 6, r.y + 21}, 6);
            mx += 18;
        }
        const float metaW = std::min(std::max(0.f, x + tw - mx), std::ceil(t.meta.measure().w) + 1);
        c.text(t.meta, {mx, r.y + 12, metaW, 18}, col.fgTertiary, gfx::VAlign::Center);
        mx += metaW + 10;
        int64_t pos = st ? st->positionMs : 0, dur = st && st->durationMs > 0 ? st->durationMs : e.durationMs;
        if (current) {
            pos = ctx().player->positionMs();
            if (const int64_t d = ctx().player->durationMs(); d > 0) dur = d;
            live = live || playing;
        }
        if (played && !current) {
            c.icon("check", {mx, r.y + 15, 12, 12}, col.fgTertiary);
            c.text(toUpperTr(tr(L"Oynatıldı")), type::monoMeta, {mx + 16, r.y + 12, 140, 18}, col.fgTertiary, gfx::TextAlign::Leading,
                   gfx::VAlign::Center);
        } else if (pos > 0 && dur > 0 && mx + 60 < x + tw) {
            const Rect bar{mx, r.y + 20, 48, 3};
            c.fillRounded(bar, 1.5f, col.hairStrong);
            c.fillRounded({bar.x, bar.y, bar.w * std::clamp(static_cast<float>(pos) / static_cast<float>(dur), 0.f, 1.f), bar.h}, 1.5f, acc.base);
            const std::wstring left = i18n::format(tr(L"{} kaldı"), {totalDuration(std::max<int64_t>(0, dur - pos))});
            c.text(toUpperTr(left), type::monoMeta, {bar.right() + 8, r.y + 12, std::max(0.f, x + tw - bar.right() - 8), 18}, col.fgTertiary,
                   gfx::TextAlign::Leading, gfx::VAlign::Center);
        }
        // Title and notes.
        const Color titleColor = current ? acc.base : played ? col.fgSecondary : col.fgPrimary;
        float badgeW = 0;
        if (e.explicitContent) badgeW = 20;
        c.text(t.title, {x, r.y + 32, std::max(0.f, tw - badgeW), 22}, titleColor, gfx::VAlign::Center);
        if (e.explicitContent) {
            const float bx = x + std::min(tw - badgeW, std::ceil(t.title.measure().w)) + 6;
            const Rect badge{bx, r.y + 36, 14, 14};
            c.fillRounded(badge, gfx::radii().badge, col.fgPrimary.withAlpha(0.14f));
            c.text(L"E", type::monoBadge, badge, col.fgSecondary, gfx::TextAlign::Center, gfx::VAlign::Center);
        }
        c.text(t.desc, {x, r.y + 58, tw, 36}, played ? col.fgTertiary : col.fgSecondary);

        // Play / pause.
        const bool playHot = hot && hoverPart_ == Part::Play;
        const gfx::Point pc{play.cx(), play.cy()};
        if (playing) {
            c.fillCircle(pc, 20, playHot ? acc.hover : acc.base);
            c.icon("pause", play.center(14, 14), acc.onAccent);
        } else {
            if (playHot) c.fillCircle(pc, 20, col.overlayHover);
            c.strokeCircle(pc, 19.5f, playHot ? col.fgSecondary : col.hairControl);
            c.icon("play", play.center(14, 14), current ? acc.base : col.fgPrimary);
        }
        // Download: arrow, progress ring (click cancels) or the downloaded mark (click deletes).
        const bool dlHot = hot && hoverPart_ == Part::Download;
        if (dlHot) c.fillCircle({dl.cx(), dl.cy()}, 16, col.overlayHover);
        if (isDownloaded(id)) {
            drawDownloadedMark(c, {dl.cx(), dl.cy()}, 9);
        } else if (auto p = downloadProgress(id)) {
            loading = true;
            c.strokeCircle({dl.cx(), dl.cy()}, 9, col.hairStrong, 2);
            if (*p > 0) c.arc({dl.cx(), dl.cy()}, 9, -90, 360 * std::clamp(*p, 0.f, 1.f), acc.base, 2);
            c.fillRounded(dl.center(6, 6), 1, col.fgSecondary);
        } else if (hot || keyboardFocused()) {
            c.icon("download", dl.center(16, 16), dlHot ? col.fgPrimary : col.fgSecondary);
        }
        const bool moreHot = hot && hoverPart_ == Part::More;
        if (moreHot) c.fillCircle({more.cx(), more.cy()}, 16, col.overlayHover);
        if (hot || keyboardFocused()) c.icon("more", more.center(16, 16), moreHot ? col.fgPrimary : col.fgSecondary);
        c.hline(r.x + 12, r.right() - 12, r.bottom() - 1, col.hairSubtle);
    }

    Options opts_;
    std::vector<Episode> episodes_;
    std::unordered_map<int, RowText> texts_;
    int hoverRow_ = -1;
    ui::HoverFade hoverFade_;   // the hovered row's background fades in / out (ui/Micro)
    Part hoverPart_ = Part::None;
    int cursor_ = 0;
};

// ===================================================================================================
// Show header: artwork, title, author, notes, then play / subscribe / more and the list's order + filter.

class ShowHeader : public HeroHeader {
public:
    ShowHeader() {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        subscribe_ = add<Button>(ButtonKind::Secondary, tr(L"Abone ol"), "plus");
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        more_->setTooltip(tr(L"Diğer seçenekler"));
        order_ = add<Button>(ButtonKind::Ghost, tr(L"En yeni önce"), "sort");
        filter_ = add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Bölümlerde ara"));
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        desc_ = gfx::Text({}, type::secondary, wrap);
    }

    void setShow(const Show& s) {
        show_ = s;
        setTitle(toWide(s.title));
        desc_.setText(snippet(s.description, 400));
        images_.clear();
        if (!s.image.empty()) images_.push_back({s.image, 0, 0});
        setTintImages(images_);   // the artwork's color, cross-faded in when it arrives
        requestLayout();
        invalidate();
    }
    void setSubscribed(bool on) {
        subscribe_->setLabel(on ? tr(L"Abonesin") : tr(L"Abone ol"));
        subscribe_->setIcon(on ? "check" : "plus");
        subscribe_->setActive(on);
        requestLayout();
    }
    void setNewestFirst(bool on) {
        order_->setLabel(on ? tr(L"En yeni önce") : tr(L"En eski önce"));
        requestLayout();
    }
    float preferredHeight(float) override { return 232 + 28 + 56 + 8; }
    void layout() override {
        const Rect r = rect();
        const float y = 232 + 28;
        play_->setRect({0, y, 56, 56});
        const float sw = subscribe_->naturalWidth();
        subscribe_->setRect({56 + 16, y + 8, sw, 40});
        more_->setRect({56 + 16 + sw + 14, y + 8, 40, 40});
        const float fw = std::min(260.f, std::max(160.f, r.w * 0.28f));
        filter_->setRect({r.w - fw, y + 6, fw, 44});
        const float ow = order_->naturalWidth();
        order_->setRect({r.w - fw - 12 - ow, y + 10, ow, 36});
        // Texts (header-local): the title beside the artwork, the notes (two lines at most), the author line.
        const float tx = kCoverSize + 32, tw = std::max(0.f, r.w - tx);
        const float th = fitTitle(tw, {72.f, 56.f, 44.f, 36.f, 28.f}, 44.f, type::displayXL);
        descW_ = std::min(tw, 640.f);
        descH_ = desc_.empty() ? 0.f : std::ceil(desc_.measure(descW_).h);
        float ty = kCoverSize - th - 30 - (desc_.empty() ? 0.f : descH_ + 6);
        labelY_ = ty - 26;
        contentTop_ = labelY_;
        titleBox_ = {tx - titleSize_ * 0.04f, ty, tw, th};
        ty += th + 10;
        descY_ = ty;
        if (!desc_.empty()) ty += descH_ + 6;
        lineY_ = ty;
    }

    Button *subscribe_, *more_, *order_;
    ui::TextBox* filter_;

protected:
    void paintContent(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float tx = r.x + kCoverSize + 32, tw = std::max(0.f, r.right() - tx);
        std::wstring label = toUpperTr(tr(L"Podcast"));
        if (!show_.categories.empty()) label += L" · " + toUpperTr(toWide(show_.categories.front()));
        c.text(label, type::monoLabel, {tx, r.y + labelY_, tw, 14}, col.fgSecondary);
        if (!desc_.empty()) c.text(desc_, {tx, r.y + descY_, descW_, descH_}, col.fgSecondary);
        std::wstring line = toWide(show_.author);
        const std::wstring count = i18n::plural(L"{} bölüm", static_cast<int>(show_.episodes.size()));
        line += line.empty() ? count : L" · " + count;
        c.text(line, type::body, {tx, r.y + lineY_, tw, 22}, col.fgPrimary, gfx::TextAlign::Leading, gfx::VAlign::Center);
    }

private:
    Show show_;
    gfx::Text desc_;
    float labelY_ = 0, descY_ = 0, descW_ = 0, descH_ = 0, lineY_ = 0;
};

// ===================================================================================================
// Now Playing notes cache (one episode at a time).

struct NotesCache {
    std::string id;
    float width = -1, height = -1;
    bool known = false;   // the store knew the episode (a restored queue may learn it later)
    gfx::Text text;
};

NotesCache& notesCache() {
    static NotesCache n;
    return n;
}

// ===================================================================================================
// The page

enum class View { Home, Search, Show, New, Downloads, SpotifyShow, SpotifyImport };

std::wstring podcastError(const std::string& raw) {
    if (raw.find("local network") != std::string::npos) return tr(L"Bu adres yerel ağda; yalnızca RSS adresiyle eklenen podcastler yerel ağdan çalar.");
    if (raw.find("not an RSS") != std::string::npos || raw.find("Atom") != std::string::npos || raw.find("XML") != std::string::npos)
        return tr(L"Bu adreste bir podcast beslemesi bulunamadı.");
    if (raw.find("HTTP 404") != std::string::npos || raw.find("HTTP 410") != std::string::npos) return tr(L"Podcast beslemesi artık yayında değil.");
    return tr(L"Podcast yüklenemedi. Bağlantını kontrol edip tekrar dene.");
}

// A show from a subscription (for its card before the feed loads).
Show showOf(const podcast::Subscription& s) {
    Show show;
    show.feedUrl = s.feedUrl;
    show.title = s.title;
    show.author = s.author;
    show.image = s.image;
    show.appleId = s.appleId;
    return show;
}

// Newest first across subscriptions, from the feeds on disk (worker thread).
std::vector<Episode> newEpisodes(const std::vector<podcast::Subscription>& subs, size_t perShow, size_t max, bool unplayedOnly,
                                 const std::unordered_map<std::string, bool>& played) {
    std::vector<Episode> all;
    for (const auto& s : subs) {
        auto show = podcast::Client::shared().cachedFeed(s.feedUrl);
        if (!show) continue;
        size_t taken = 0;
        for (auto& e : show->episodes) {
            if (taken >= perShow) break;
            if (unplayedOnly) {
                const auto it = played.find(e.id());
                if (it != played.end() && it->second) continue;
            }
            all.push_back(std::move(e));
            ++taken;
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Episode& a, const Episode& b) { return a.published > b.published; });
    if (all.size() > max) all.resize(max);
    return all;
}

class PodcastsPage : public ScrollPage {
public:
    explicit PodcastsPage(const std::string& id) {
        parseRoute(id);
        enableCollapse();   // a show's header collapses into a compact bar (app/CollapsingHeader)
        podcast::store().subscribe(subs_.ref(), [this] { scheduleStoreRebuild(); });
        spotifyshows::subscribe(subs_.ref(), [this] { scheduleSpotifyRefresh(); });
        build();
    }
    ~PodcastsPage() override { cts_->cancel(); }

    // Page transitions: a show's header cover (HeaderCover, which the collapsing bar moves; while its feed loads: where
    // it will be).
    bool coverSlot(SharedCover& out) override {
        if (view_ != View::Show && !(view_ == View::SpotifyShow && header_)) return false;
        if (header_) return headerCoverSlot(out);
        return loadingCoverSlot(out);
    }

    void paint(Canvas& c) override {
        syncHeaderPlay();   // the show's play button follows the player
        ScrollPage::paint(c);
        // Search debounce: 450 ms after the last keystroke.
        if (!pending_.empty() && ui::frame::realNow() >= pendingAt_) {
            const std::wstring q = pending_;
            pending_.clear();
            startSearch(q);
        } else if (!pending_.empty()) {
            if (auto* w = window()) w->invalidateAfter(pendingAt_ - ui::frame::realNow() + 1);
        }
    }

private:
    // ---- route --------------------------------------------------------------------------------------------------
    void parseRoute(const std::string& id) {
        auto starts = [&](std::string_view prefix) {
            if (id.rfind(prefix, 0) != 0) return false;
            arg_ = id.substr(prefix.size());
            return true;
        };
        if (id.rfind("spotify:show:", 0) == 0) {
            view_ = View::SpotifyShow;
            spotifyUri_ = id;
            return;
        }
        if (id == "spotify") {
            view_ = View::SpotifyImport;
            return;
        }
        if (starts("feed:")) view_ = View::Show;
        else if (starts("apple:")) {
            view_ = View::Show;
            appleId_ = arg_;
            arg_.clear();
        } else if (starts("search:")) view_ = View::Search;
        else if (id == "new") view_ = View::New;
        else if (id == "downloads") view_ = View::Downloads;
        else view_ = View::Home;
        if (view_ == View::Show && arg_.empty() && appleId_.empty()) view_ = View::Home;
        if (view_ == View::Search && arg_.empty()) view_ = View::Home;
        if (view_ == View::Search) query_ = toWide(arg_);
    }

    // ---- build ----------------------------------------------------------------------------------------------------
    void build() {
        cts_->cancel();
        cts_ = std::make_shared<CancelSource>();
        life_.renew();
        // Every widget pointer goes with the old content (a rebuild shows a skeleton or an error first).
        header_ = nullptr;
        list_ = nullptr;
        empty_ = nullptr;
        body_ = searchSec_ = progressSec_ = newSec_ = subsSec_ = chartsSec_ = spotifySec_ = nullptr;
        importRows_.clear();
        importSummary_ = nullptr;
        importButton_ = nullptr;
        waitingForLink_ = false;
        switch (view_) {
        case View::Show: buildShowLoading(); return;
        case View::New:
        case View::Downloads: buildEpisodePage(); return;
        case View::SpotifyShow: buildSpotifyShow(); return;
        case View::SpotifyImport: buildSpotifyImport(); return;
        default: buildHome(); return;
        }
    }

    ui::Column* sectionColumn(ui::Column* parent) { return parent->add<ui::Column>(16.f); }

    // A row of chips / buttons, left to right, 8 DIP apart.
    static void layoutChips(ui::Box* row, float h = 32.f) {
        row->hitTestVisible = false;
        row->onPreferredHeight = [h](float) { return h; };
        row->onLayout = [h](ui::Box& b) {
            float x = 0;
            for (const auto& child : b.children()) {
                if (!child->visible()) continue;
                auto* btn = static_cast<Button*>(child.get());
                const float w = btn->naturalWidth();
                btn->setRect({x, 0, w, h});
                x += w + 8;
            }
        };
    }

    void buildHome() {
        auto* c = resetContent(24.f);
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"Podcastler"), type::displayM);
        title->setVAlign(gfx::VAlign::Center);
        auto* box = top->add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Podcast ara"));
        box->setText(query_);
        box->onChange = [this](const std::wstring& s) { scheduleSearch(s); };
        box->onSubmit = [this](const std::wstring& s) {
            pending_.clear();
            startSearch(s);
        };
        box->onEscape = [box] { box->setText({}, true); };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, box](ui::Box& b) {
            const float w = b.rect().w, h = b.rect().h;
            const float bw = std::clamp(w * 0.4f, 220.f, 380.f);
            box->setRect({w - bw, h * 0.5f - gfx::metrics::searchH * 0.5f, bw, gfx::metrics::searchH});
            title->setRect({0, 0, std::max(0.f, w - bw - 24), h});
        };
        auto* kicker = c->add<ui::Label>(toUpperTr(tr(L"Podcastler")) + L" · APPLE PODCASTS · RSS", type::monoLabel, ui::Tone::Tertiary);
        c->setSpacingBefore(kicker, 2);
        auto* chips = c->add<ui::Box>();
        chips->hitTestVisible = false;
        auto* add = chips->add<Button>(ButtonKind::Chip, tr(L"RSS adresiyle ekle"), "plus");
        add->onClick = [] { promptAddFeed(); };
        auto* refresh = chips->add<Button>(ButtonKind::Chip, tr(L"Abonelikleri yenile"), "refresh");
        refresh->onClick = [] { refreshSubscriptions(true); };
        auto* downloads = chips->add<Button>(ButtonKind::Chip, tr(L"İndirilen bölümler"), "download");
        downloads->onClick = [] { ctx().router->navigate({RouteKind::Podcasts, "downloads"}); };
        if (source::loggedIn()) {
            auto* spotify = chips->add<Button>(ButtonKind::Chip, tr(L"Spotify podcastlerini içe aktar"), "spotify-link");
            spotify->onClick = [] { ctx().router->navigate({RouteKind::Podcasts, "spotify"}); };
        }
        layoutChips(chips);
        body_ = c->add<ui::Column>(gfx::metrics::sectionGap);
        c->setSpacingBefore(body_, 8);
        searchSec_ = sectionColumn(body_);
        progressSec_ = sectionColumn(body_);
        newSec_ = sectionColumn(body_);
        subsSec_ = sectionColumn(body_);
        spotifySec_ = sectionColumn(body_);
        chartsSec_ = sectionColumn(body_);
        for (auto* s : {searchSec_, progressSec_, newSec_, spotifySec_}) s->setVisible(false);   // empty sections take no gap
        buildStoreSections();
        loadCharts();
        if (!query_.empty()) startSearch(query_);
        contentReady();
    }

    // Sections that follow the store: continue listening, new episodes, subscriptions.
    void buildStoreSections() {
        if (view_ != View::Home || !progressSec_) return;
        progressSec_->clearChildren();
        const auto progress = podcast::store().inProgress(4);
        progressSec_->setVisible(!progress.empty());
        if (!progress.empty()) {
            progressSec_->add<SectionHeader>(tr(L"Kaldığın yerden"), toUpperTr(tr(L"Devam et")));
            progressSec_->add<EpisodeList>(EpisodeList::Options{true, true})->setEpisodes(progress);
        }
        subsSec_->clearChildren();
        const auto& subs = podcast::store().subscriptions();
        subsSec_->add<SectionHeader>(tr(L"Aboneliklerin"), subs.empty() ? std::wstring() : i18n::number(static_cast<int>(subs.size())));
        if (subs.empty()) {
            subsSec_->add<ui::Label>(tr(L"Henüz aboneliğin yok. Aşağıdaki popüler podcastlerden birini aç ya da yukarıdan ara."), type::secondary,
                                     ui::Tone::Secondary)
                ->setWrap(true);
        } else {
            auto* grid = addCardRow(subsSec_, 150, 0);
            for (const auto& s : subs) {
                std::vector<catalog::Image> images;
                if (!s.image.empty()) images.push_back({s.image, 0, 0});
                std::wstring sub = toWide(s.author);
                if (s.newEpisodes > 0) sub = i18n::plural(L"{} yeni bölüm", s.newEpisodes);
                auto* card = grid->add<MediaCard>(toWide(s.title), sub, images);
                const std::string feed = s.feedUrl;
                card->onOpen = [feed] { openShow(feed); };
                card->onPlay = [feed] { playLatest(feed); };
                const Show show = showOf(s);
                card->onContext = [show](gfx::Point p) { ui::Menu::open(ctx().window, p, showMenuItems(show)); };
            }
        }
        loadNew();
        buildSpotifySection();
        scroll_->contentChanged();
    }

    // "Spotify'daki podcastlerin" (a Spotify session): the followed shows as cards saying what they resolved to; the
    // lookups of those not resolved yet run in the background. "İçe aktar" opens the preview.
    void buildSpotifySection() {
        if (view_ != View::Home || !spotifySec_) return;
        using spotifyshows::ListState;
        spotifyshows::ensureFollowed();
        spotifySec_->clearChildren();
        const auto state = spotifyshows::listState();
        const auto& shows = spotifyshows::followed();
        const bool visible = state != ListState::NoSession && !(state == ListState::Ready && shows.empty());
        spotifySec_->setVisible(visible);
        if (!visible) return;
        auto* h = spotifySec_->add<SectionHeader>(tr(L"Spotify'daki podcastlerin"),
                                                  shows.empty() ? std::wstring() : i18n::number(static_cast<int>(shows.size())),
                                                  tr(L"İçe aktar"));
        h->onLink = [] { ctx().router->navigate({RouteKind::Podcasts, "spotify"}); };
        if (state == ListState::Failed) {
            spotifySec_->add<ui::Label>(tr(L"Spotify podcastlerin yüklenemedi."), type::secondary, ui::Tone::Secondary);
            return;
        }
        if (shows.empty()) {
            spotifySec_->add<SkeletonBlock>(0);
            return;
        }
        auto* grid = addCardRow(spotifySec_, 150, 1);
        for (const auto& s : shows) {
            auto* card = grid->add<MediaCard>(toWide(s.name), spotifyshows::cardSubtitle(s), s.images);
            card->onOpen = [id = s.id] { ctx().router->navigate({RouteKind::Podcasts, id}); };
        }
        spotifyshows::resolveAll(shows);
    }

    void scheduleStoreRebuild() {
        if (rebuildPosted_) return;
        rebuildPosted_ = true;
        // Deferred: the change may come from an event of a widget these sections own.
        Dispatcher::post([ref = subs_.ref(), this] {
            if (ref.expired()) return;
            rebuildPosted_ = false;
            if (view_ == View::Home) buildStoreSections();
            else if ((view_ == View::Show || view_ == View::SpotifyShow) && header_)
                header_->setSubscribed(podcast::store().isSubscribed(show_.feedUrl));
            else if (view_ == View::Downloads) buildEpisodePage();
            else if (view_ == View::SpotifyImport) refreshImport();
            invalidate();
        });
    }

    // The followed list arrived or a lookup finished: only what shows Spotify's shows.
    void scheduleSpotifyRefresh() {
        if (spotifyPosted_) return;
        spotifyPosted_ = true;
        Dispatcher::post([ref = subs_.ref(), this] {
            if (ref.expired()) return;
            spotifyPosted_ = false;
            if (view_ == View::Home) {
                buildSpotifySection();
                scroll_->contentChanged();
            } else if (view_ == View::SpotifyImport) {
                refreshImport();
            } else if (view_ == View::SpotifyShow && waitingForLink_) {
                const auto l = spotifyshows::lookup(spotifyUri_);
                if (l != spotifyshows::Lookup::Pending && l != spotifyshows::Lookup::Unknown) build();
            }
            invalidate();
        });
    }

    void loadNew() {
        newLife_.renew();   // a newer load replaces one still running
        const auto subs = podcast::store().subscriptions();
        if (subs.empty()) {
            newSec_->clearChildren();
            newSec_->setVisible(false);
            return;
        }
        std::unordered_map<std::string, bool> played;
        async(
            Priority::Normal, newLife_.ref(), [subs, played] { return newEpisodes(subs, 3, 6, true, played); },
            [this](Result<std::vector<Episode>> r) {
                newSec_->clearChildren();
                std::vector<Episode> list;
                if (r)   // not heard yet, and not already under "Kaldığın yerden"
                    for (auto& e : *r)
                        if (const auto* st = podcast::store().state(e.id()); !st || (!st->played && st->positionMs == 0)) list.push_back(std::move(e));
                newSec_->setVisible(!list.empty());
                if (!list.empty()) {
                    auto* h = newSec_->add<SectionHeader>(tr(L"Yeni bölümler"), toUpperTr(tr(L"Aboneliklerinden")), tr(L"Tümü"));
                    h->onLink = [] { ctx().router->navigate({RouteKind::Podcasts, "new"}); };
                    newSec_->add<EpisodeList>(EpisodeList::Options{true, true})->setEpisodes(std::move(list));
                }
                scroll_->contentChanged();
                invalidate();
            });
    }

    void loadCharts() {
        chartsSec_->clearChildren();
        const std::string cc = homeCountry();
        chartsSec_->add<SectionHeader>(tr(L"Popüler podcastler"), toUpperTr(tr(L"Apple Podcasts listesi")));
        auto* grid = addCardRow(chartsSec_, 150, 0);
        auto* skeleton = chartsSec_->add<SkeletonBlock>(0);
        auto cts = cts_;
        async(
            Priority::Normal, life_.ref(), [cc, cts] { return podcast::Client::shared().topCharts(cc, 30, cts->token()); },
            [this, grid, skeleton](Result<std::vector<DirectoryEntry>> r) {
                chartsSec_->removeChild(skeleton);
                if (!r || r->empty()) {
                    chartsSec_->add<ui::Label>(tr(L"Popüler podcastler yüklenemedi."), type::secondary, ui::Tone::Secondary);
                } else {
                    for (const auto& e : *r) addDirectoryCard(grid, e);
                }
                scroll_->contentChanged();
                invalidate();
            });
    }

    static void addDirectoryCard(ui::Widget* grid, const DirectoryEntry& e) {
        std::vector<catalog::Image> images;
        if (!e.image.empty()) images.push_back({e.image, 0, 0});
        auto* card = grid->add<MediaCard>(toWide(e.title), toWide(e.author), images);
        const std::string route = !e.feedUrl.empty() ? "feed:" + e.feedUrl : "apple:" + e.appleId;
        card->onOpen = [route] { ctx().router->navigate({RouteKind::Podcasts, route}); };
    }

    void scheduleSearch(const std::wstring& s) {
        query_ = s;
        if (!searchSec_) return;
        if (s.empty()) {
            pending_.clear();
            searchSec_->clearChildren();
            searchSec_->setVisible(false);
            scroll_->contentChanged();
            return;
        }
        pending_ = s;
        pendingAt_ = ui::frame::realNow() + 450;
        invalidate();
    }

    void startSearch(const std::wstring& q) {
        std::string text = toUtf8(q);
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (text.empty() || !searchSec_) return;
        searchSec_->setVisible(true);
        searchSec_->clearChildren();
        searchSec_->add<SectionHeader>(i18n::format(tr(L"“{}” için sonuçlar"), {toWide(text)}), toUpperTr(tr(L"Apple Podcasts dizini")));
        auto* grid = addCardRow(searchSec_, 150, 0);
        auto* skeleton = searchSec_->add<SkeletonBlock>(0);
        scroll_->contentChanged();
        searchLife_.renew();
        const std::string cc = homeCountry();
        async(
            Priority::High, searchLife_.ref(), [text, cc] { return podcast::Client::shared().search(text, cc, 30); },
            [this, grid, skeleton](Result<std::vector<DirectoryEntry>> r) {
                searchSec_->removeChild(skeleton);
                if (!r) searchSec_->add<ui::Label>(tr(L"Arama yapılamadı. Bağlantını kontrol et."), type::secondary, ui::Tone::Secondary);
                else if (r->empty()) searchSec_->add<ui::Label>(tr(L"Podcast bulunamadı"), type::secondary, ui::Tone::Secondary);
                else
                    for (const auto& e : *r) addDirectoryCard(grid, e);
                scroll_->contentChanged();
                invalidate();
            });
    }

    // ---- New / Downloads lists --------------------------------------------------------------------------------
    void buildEpisodePage() {
        auto* c = resetContent(24.f);
        const bool downloads = view_ == View::Downloads;
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(downloads ? tr(L"İndirilen bölümler") : tr(L"Yeni bölümler"), type::displayS);
        title->setVAlign(gfx::VAlign::Center);
        auto* back = top->add<Button>(ButtonKind::Ghost, tr(L"Podcastler"), "arrow-back");
        back->onClick = [] { ctx().router->navigate({RouteKind::Podcasts}); };
        top->onPreferredHeight = [](float) { return 56.f; };
        top->onLayout = [title, back](ui::Box& b) {
            const float w = b.rect().w, h = b.rect().h, bw = back->naturalWidth();
            back->setRect({w - bw, h * 0.5f - 18, bw, 36});
            title->setRect({0, 0, std::max(0.f, w - bw - 24), h});
        };
        c->add<ui::Label>(toUpperTr(downloads ? tr(L"Bu bilgisayarda") : tr(L"Aboneliklerinden")), type::monoLabel, ui::Tone::Tertiary);
        if (downloads) {
            const auto list = podcast::store().downloaded();
            if (list.empty()) c->add<MessagePanel>("download", tr(L"İndirilen bölüm yok"), tr(L"Bir bölümün yanındaki ok ile çevrimdışı dinlemek için indir."));
            else c->add<EpisodeList>(EpisodeList::Options{true, true})->setEpisodes(list);
            contentReady();
            return;
        }
        auto* skeleton = c->add<SkeletonBlock>(4);
        const auto subs = podcast::store().subscriptions();
        std::unordered_map<std::string, bool> played;
        async(
            Priority::High, life_.ref(), [subs, played] { return newEpisodes(subs, 20, 200, false, played); },
            [this, c, skeleton](Result<std::vector<Episode>> r) {
                c->removeChild(skeleton);
                if (!r || r->empty())
                    c->add<MessagePanel>("offline", tr(L"Yeni bölüm yok"), tr(L"Abone olduğun podcastlerin bölümleri burada görünür."));
                else c->add<EpisodeList>(EpisodeList::Options{true, true})->setEpisodes(std::move(*r));
                scroll_->contentChanged();
                contentReady();
            });
    }

    // ---- Show --------------------------------------------------------------------------------------------------------
    void buildShowLoading() {
        showSkeleton(6);
        const std::string feedUrl = arg_, appleId = appleId_;
        const bool local = allowLocal(feedUrl);
        auto cts = cts_;
        async(
            Priority::High, life_.ref(),
            [feedUrl, appleId, local, cts]() -> Show {
                auto& client = podcast::Client::shared();
                std::string url = feedUrl;
                if (url.empty()) url = client.lookupFeed(appleId, cts->token());
                if (url.empty()) throw podcast::PodcastError("not an RSS feed (the directory has none)");
                try {
                    Show s = client.feed(url, false, local, cts->token());
                    if (s.appleId.empty()) s.appleId = appleId;
                    return s;
                } catch (const podcast::PodcastError& e) {
                    // Offline or the host is down: the copy on disk, however old.
                    if (auto cached = client.cachedFeed(url)) {
                        ST_LOG_WARN("podcasts", "feed failed ({}); showing the cached copy", e.what());
                        return std::move(*cached);
                    }
                    throw;
                }
            },
            [this](Result<Show> r) {
                if (!r) {
                    showError(podcastError(r.errorMessage()), [this] { build(); });
                    return;
                }
                buildShow(std::move(*r));
            });
    }

    void buildShow(Show show) {
        show_ = std::move(show);
        podcast::store().remember(show_.episodes);
        if (podcast::store().isSubscribed(show_.feedUrl)) {
            podcast::store().refreshed(show_, nowUnix());
            int64_t newest = 0;
            for (const auto& e : show_.episodes) newest = std::max(newest, e.published);
            podcast::store().markSeen(show_.feedUrl, newest);
        }
        auto* c = resetContent(24.f);
        header_ = c->add<ShowHeader>();
        header_->setShow(show_);
        header_->setSubscribed(podcast::store().isSubscribed(show_.feedUrl));
        header_->setNewestFirst(newestFirst_);
        header_->play_->onClick = [this] { playShow(); };
        header_->subscribe_->onClick = [this] {
            auto& st = podcast::store();
            if (st.isSubscribed(show_.feedUrl)) {
                st.unsubscribe(show_.feedUrl);
                toast(tr(L"Abonelikten çıkıldı"));
            } else {
                st.subscribe(show_, false, nowUnix());
                toast(tr(L"Abone olundu"));
            }
        };
        header_->more_->onClick = [this] {
            const Rect b = header_->more_->toWindow(header_->more_->rect());
            auto items = showMenuItems(show_);
            items.push_back(ui::MenuItem::sep());
            items.push_back({tr(L"Yenile"), "refresh", L"", [this] {
                                 podcast::Client::shared().dropCachedFeed(show_.feedUrl);
                                 build();
                             }});
            if (!spotifyUri_.empty()) {   // opened as a Spotify show: this feed is the directory's match for it
                items.push_back(ui::MenuItem::sep());
                items.push_back({tr(L"Spotify'da aç"), "external-link", L"", [url = spotifyshows::webUrl(spotifyUri_)] {
                                     ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                 }});
                items.push_back({tr(L"Yanlış podcast mi?"), "swap-match", L"", [this] {
                                     spotifyshows::reject(spotifyUri_);
                                     toast(tr(L"Eşleşme kaldırıldı"));
                                     Dispatcher::post([this, ref = subs_.ref()] {
                                         if (!ref.expired()) build();
                                     });
                                 }});
            }
            ui::Menu::open(ctx().window, {b.x, b.bottom() + 4}, std::move(items));
        };
        header_->order_->onClick = [this] {
            newestFirst_ = !newestFirst_;
            header_->setNewestFirst(newestFirst_);
            applyFilter();
        };
        header_->filter_->onChange = [this](const std::wstring& s) {
            filter_ = foldForSearch(s);
            applyFilter();
        };
        header_->filter_->onEscape = [this] { header_->filter_->setText({}, true); };
        list_ = c->add<EpisodeList>(EpisodeList::Options{false, false});
        empty_ = c->add<ui::Label>(tr(L"Bu podcastte çalınabilir bölüm yok."), type::secondary, ui::Tone::Secondary);
        attachHeader(header_, list_);
        applyFilter();
        syncHeaderPlay();
        contentReady();
    }

    void applyFilter() {
        if (!list_) return;
        std::vector<Episode> view;
        view.reserve(show_.episodes.size());
        for (const auto& e : show_.episodes)
            if (filter_.empty() || foldForSearch(toWide(e.title)).find(filter_) != std::wstring::npos ||
                foldForSearch(toWide(e.description.substr(0, 2000))).find(filter_) != std::wstring::npos)
                view.push_back(e);
        if (!newestFirst_) std::reverse(view.begin(), view.end());
        empty_->setVisible(view.empty());
        if (!filter_.empty() && view.empty()) empty_->setText(tr(L"Aramana uyan bölüm yok."));
        list_->setEpisodes(std::move(view));
        scroll_->contentChanged();
    }

    // The show's header play button: the episode in progress, else the newest one not heard yet, else the newest.
    void playShow() {
        const auto* cur = ctx().player ? ctx().player->current() : nullptr;
        if (cur && catalog::isPodcastId(cur->id)) {
            if (const Episode* e = podcast::store().find(cur->id); e && e->feedUrl == show_.feedUrl) {
                ctx().player->togglePause();
                return;
            }
        }
        if (show_.episodes.empty()) return;
        for (const auto& e : podcast::store().inProgress(50))
            if (e.feedUrl == show_.feedUrl) return playEpisode(e);
        for (const auto& e : show_.episodes)
            if (!podcast::store().isPlayed(e.id())) return playEpisode(e);
        playEpisode(show_.episodes.front());
    }

    void syncHeaderPlay() {
        if (!header_) return;
        bool mine = false;
        if (const auto* cur = ctx().player ? ctx().player->current() : nullptr; cur && catalog::isPodcastId(cur->id))
            if (const Episode* e = podcast::store().find(cur->id)) mine = e->feedUrl == show_.feedUrl;
        header_->play_->setPlaying(mine && ctx().player->isPlaying());
    }

    // ---- Spotify shows -----------------------------------------------------------------------------------------------
    // The show as Spotify names it: seen this session, else what the store kept with its last lookup.
    std::optional<catalog::PodcastShow> spotifyInfo() const {
        if (const auto* s = spotifyshows::known(spotifyUri_)) return *s;
        if (const auto* l = podcast::store().spotifyLink(spotifyUri_); l && !l->showName.empty()) {
            catalog::PodcastShow s;
            s.id = spotifyUri_;
            s.name = l->showName;
            s.publisher = l->showPublisher;
            if (!l->showImage.empty()) s.images.push_back({l->showImage, 0, 0});
            return s;
        }
        return std::nullopt;
    }

    // A matched show is its feed's page; one without a public feed says so; else the lookup runs (urgent) and the page
    // waits for it on a skeleton (scheduleSpotifyRefresh rebuilds once there is a verdict).
    void buildSpotifyShow() {
        using spotifyshows::Lookup;
        const Lookup l = spotifyshows::lookup(spotifyUri_);
        if (l == Lookup::Matched) {
            arg_ = podcast::store().spotifyLink(spotifyUri_)->feedUrl;
            buildShowLoading();
            return;
        }
        if (l == Lookup::NoFeed) return buildSpotifyUnavailable();
        if (l == Lookup::Failed) {
            showError(tr(L"Podcast dizinine ulaşılamadı. Bağlantını kontrol edip tekrar dene."), [this] { lookUpAgain(); });
            return;
        }
        showSkeleton(6);
        waitingForLink_ = true;
        if (l == Lookup::Pending) return;
        if (auto info = spotifyInfo()) return spotifyshows::resolve(*info, true);
        spotify::Api* api = source::activeApi();
        if (!api) {
            waitingForLink_ = false;
            showError(tr(L"Bu podcasti açmak için Spotify'a bağlan."), [this] { build(); });
            return;
        }
        async(Priority::High, life_.ref(), [api, uri = spotifyUri_] { return api->show(uri); },
              [this](Result<catalog::PodcastShow> r) {
                  if (!r) {
                      waitingForLink_ = false;
                      showError(tr(L"Podcast yüklenemedi. Bağlantını kontrol edip tekrar dene."), [this] { build(); });
                      return;
                  }
                  spotifyshows::resolve(*r, true);
              });
    }

    void lookUpAgain() {
        if (auto info = spotifyInfo()) spotifyshows::resolve(*info, true, true);
        build();
    }

    // No public feed: the show as Spotify has it, why it can't play here and what the user can do instead.
    void buildSpotifyUnavailable() {
        const auto info = spotifyInfo();
        const std::wstring name = info ? toWide(info->name) : std::wstring();
        const std::wstring publisher = info ? toWide(info->publisher) : std::wstring();
        const std::vector<catalog::Image> images = info ? info->images : std::vector<catalog::Image>{};
        auto* c = resetContent(20.f);
        auto* head = c->add<ui::Box>();
        head->hitTestVisible = false;
        head->onPreferredHeight = [](float) { return 184.f; };
        head->onPaint = [name, publisher, images, title = gfx::Text(name, type::displayS), sub = gfx::Text(publisher, type::body)](
                            ui::Box& b, Canvas& cv) mutable {
            const Rect r = b.rect();
            const auto& col = colors();
            drawArtwork(cv, images, {r.x, r.y, 184, 184}, gfx::radii().art, Placeholder::Album);
            const float tx = r.x + 184 + 28, tw = std::max(0.f, r.right() - tx);
            cv.text(toUpperTr(tr(L"Podcast")) + L" · SPOTIFY", type::monoLabel, {tx, r.y + 72, tw, 14}, col.fgSecondary);
            cv.text(title, {tx, r.y + 92, tw, 44}, col.fgPrimary);
            cv.text(sub, {tx, r.y + 140, tw, 22}, col.fgSecondary);
        };
        auto* heading = c->add<ui::Label>(tr(L"Bu podcast ShadeTube'da çalınamaz"), type::sectionTitle);
        c->setSpacingBefore(heading, 32);
        auto* why = c->add<ui::Label>(tr(L"Herkese açık bir RSS beslemesi bulunamadı; büyük olasılıkla yalnızca Spotify'da yayınlanıyor. "
                                         L"ShadeTube Spotify'ın sesini çalamaz, podcastleri herkese açık beslemelerinden çalar."),
                                      type::secondary, ui::Tone::Secondary);
        why->setWrap(true);
        if (const auto* l = podcast::store().spotifyLink(spotifyUri_); l && !l->rejectedFeed.empty())
            c->add<ui::Label>(tr(L"Dizinin önerdiği eşleşmeyi yanlış olarak işaretledin."), type::caption, ui::Tone::Tertiary);
        auto* row = c->add<ui::Box>();
        auto* open = row->add<Button>(ButtonKind::Secondary, tr(L"Spotify'da aç"), "external-link");
        open->onClick = [url = spotifyshows::webUrl(spotifyUri_)] { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        if (!name.empty()) {
            auto* search = row->add<Button>(ButtonKind::Ghost, tr(L"Apple Podcasts'ta ara"), "search");
            search->onClick = [q = info->name] { ctx().router->navigate({RouteKind::Podcasts, "search:" + q}); };
            auto* again = row->add<Button>(ButtonKind::Ghost, tr(L"Tekrar ara"), "refresh");
            again->onClick = [this] {
                Dispatcher::post([this, ref = subs_.ref()] {
                    if (!ref.expired()) lookUpAgain();
                });
            };
        }
        auto* add = row->add<Button>(ButtonKind::Ghost, tr(L"RSS adresiyle ekle"), "plus");
        add->onClick = [] { promptAddFeed(); };
        layoutChips(row, 40.f);
        c->setSpacingBefore(row, 16);
        contentReady();
    }

    // ---- Spotify import preview -----------------------------------------------------------------------------------
    void buildSpotifyImport() {
        using spotifyshows::ListState;
        spotifyshows::ensureFollowed();
        importState_ = spotifyshows::listState();
        const auto& shows = spotifyshows::followed();
        auto* c = resetContent(20.f);
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"Spotify podcastlerin"), type::displayS);
        title->setVAlign(gfx::VAlign::Center);
        auto* back = top->add<Button>(ButtonKind::Ghost, tr(L"Podcastler"), "arrow-back");
        back->onClick = [] { ctx().router->navigate({RouteKind::Podcasts}); };
        top->onPreferredHeight = [](float) { return 56.f; };
        top->onLayout = [title, back](ui::Box& b) {
            const float w = b.rect().w, h = b.rect().h, bw = back->naturalWidth();
            back->setRect({w - bw, h * 0.5f - 18, bw, 36});
            title->setRect({0, 0, std::max(0.f, w - bw - 24), h});
        };
        std::wstring kicker = L"SPOTIFY";
        if (!shows.empty()) kicker += L" · " + toUpperTr(i18n::plural(L"{} podcast", static_cast<long long>(shows.size())));
        c->add<ui::Label>(kicker, type::monoLabel, ui::Tone::Tertiary);
        auto* note = c->add<ui::Label>(tr(L"ShadeTube Spotify'ın sesini çalamaz: podcastler herkese açık RSS beslemelerinden çalar. Her podcast "
                                          L"Apple Podcasts dizininde adı ve yayıncısıyla aranır; eşleşenlerden seçtiklerine abone olursun."),
                                       type::secondary, ui::Tone::Secondary);
        note->setWrap(true);
        switch (importState_) {
        case ListState::NoSession:
            c->add<MessagePanel>("spotify-link", tr(L"Spotify'a bağlı değilsin"),
                                 tr(L"Spotify'da takip ettiğin podcastleri görmek için Spotify hesabınla bağlan."));
            return contentReady();
        case ListState::Loading:
            c->add<SkeletonBlock>(4);
            return contentReady();
        case ListState::Failed:
            c->add<MessagePanel>("warning", tr(L"Spotify podcastlerin yüklenemedi."), tr(L"Bağlantını kontrol edip tekrar dene."),
                                 tr(L"Tekrar dene"), [] { spotifyshows::ensureFollowed(true); });
            return contentReady();
        case ListState::Ready: break;
        }
        if (shows.empty()) {
            c->add<MessagePanel>("podcast", tr(L"Spotify'da takip ettiğin podcast yok"),
                                 tr(L"Spotify'da bir podcasti takip ettiğinde burada görünür."));
            return contentReady();
        }
        auto* actions = c->add<ui::Box>();
        importButton_ = actions->add<Button>(ButtonKind::Primary, L"", "plus");
        importButton_->onClick = [this] {
            std::vector<std::string> uris;
            for (const auto* row : importRows_)
                if (row->checked() && spotifyshows::importable(row->show().id)) uris.push_back(row->show().id);
            spotifyshows::importShows(uris);
            updateImportSummary();
        };
        auto* all = actions->add<Button>(ButtonKind::Ghost, tr(L"Tümünü seç"), "check");
        all->onClick = [this] { selectAllImports(true); };
        auto* none = actions->add<Button>(ButtonKind::Ghost, tr(L"Seçimi temizle"), "close");
        none->onClick = [this] { selectAllImports(false); };
        layoutChips(actions, 40.f);
        importSummary_ = c->add<ui::Label>(L"", type::caption, ui::Tone::Tertiary);
        auto* list = c->add<ui::Column>(0.f);
        for (const auto& s : shows) {
            auto* row = list->add<spotifyshows::ShowRow>(s);
            const std::string uri = s.id;
            row->setChecked(spotifyshows::importable(uri) && !unchecked_.contains(uri));
            row->onCheck = [this, uri](bool on) {
                if (on) unchecked_.erase(uri);
                else unchecked_.insert(uri);
                updateImportSummary();
            };
            row->onOpen = [uri] { ctx().router->navigate({RouteKind::Podcasts, uri}); };
            importRows_.push_back(row);
        }
        spotifyshows::resolveAll(shows);
        updateImportSummary();
        contentReady();
    }

    void selectAllImports(bool on) {
        for (auto* row : importRows_) {
            const std::string& uri = row->show().id;
            if (on) unchecked_.erase(uri);
            else unchecked_.insert(uri);
            row->setChecked(on && spotifyshows::importable(uri));
        }
        updateImportSummary();
    }

    // Lookups finished / subscriptions changed: the rows in place (a new list or list state: rebuilt).
    void refreshImport() {
        if (importState_ != spotifyshows::listState() || importRows_.size() != spotifyshows::followed().size()) {
            build();
            return;
        }
        for (auto* row : importRows_) {
            row->refresh();
            row->setChecked(spotifyshows::importable(row->show().id) && !unchecked_.contains(row->show().id));
        }
        updateImportSummary();
    }

    void updateImportSummary() {
        using spotifyshows::Lookup;
        int matched = 0, subscribed = 0, noFeed = 0, pending = 0, failed = 0, selected = 0;
        for (const auto* row : importRows_) {
            const std::string& uri = row->show().id;
            switch (spotifyshows::lookup(uri)) {
            case Lookup::Matched:
                if (spotifyshows::subscribed(uri)) ++subscribed;
                else ++matched;
                if (row->checked() && spotifyshows::importable(uri)) ++selected;
                break;
            case Lookup::NoFeed: ++noFeed; break;
            case Lookup::Failed: ++failed; break;
            default: ++pending; break;
            }
        }
        std::wstring s;
        auto part = [&](int n, const std::wstring& text) {
            if (n > 0) s += (s.empty() ? L"" : L" · ") + text;
        };
        part(matched, i18n::plural(L"{} abone olunabilir", matched));
        part(subscribed, i18n::plural(L"{} zaten abonelik", subscribed));
        part(noFeed, i18n::plural(L"{} herkese açık beslemesiz", noFeed));
        part(pending, i18n::plural(L"{} aranıyor", pending));
        part(failed, i18n::plural(L"{} kontrol edilemedi", failed));
        if (importSummary_) importSummary_->setText(toUpperTr(s));
        if (importButton_) {
            importButton_->setLabel(i18n::format(tr(L"Seçilenlere abone ol ({})"), {std::to_wstring(selected)}));
            importButton_->setEnabled(selected > 0 && !spotifyshows::importing());
            if (auto* p = importButton_->parent()) p->requestLayout();
        }
        scroll_->contentChanged();
    }

public:
    static std::vector<ui::MenuItem> showMenuItems(const Show& show) {
        std::vector<ui::MenuItem> items;
        const bool subscribed = podcast::store().isSubscribed(show.feedUrl);
        items.push_back({subscribed ? tr(L"Abonelikten çık") : tr(L"Abone ol"), subscribed ? "minus" : "plus", L"", [show, subscribed] {
                             if (subscribed) podcast::store().unsubscribe(show.feedUrl);
                             else podcast::store().subscribe(show, false, nowUnix());
                             toast(subscribed ? tr(L"Abonelikten çıkıldı") : tr(L"Abone olundu"));
                         }});
        if (podcast::isHttpUrl(show.link)) {
            const std::wstring url = toWide(show.link);
            items.push_back({tr(L"Web sitesini aç"), "external-link", L"", [url] { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }});
        }
        items.push_back({tr(L"RSS adresini kopyala"), "link", L"", [url = show.feedUrl] {
                             copyText(toWide(url));
                             toast(tr(L"Adres kopyalandı"));
                         }});
        return items;
    }

    // The newest episode not heard yet of a subscription (its card's play button).
    static void playLatest(const std::string& feedUrl) {
        const bool local = allowLocal(feedUrl);
        static Lifetime life;
        async(
            Priority::High, life.ref(), [feedUrl, local] { return podcast::Client::shared().feed(feedUrl, false, local); },
            [](Result<Show> r) {
                if (!r || r->episodes.empty()) {
                    toast(tr(L"Podcast yüklenemedi"), true);
                    return;
                }
                podcast::store().remember(r->episodes);
                for (const auto& e : podcast::store().inProgress(50))
                    if (e.feedUrl == r->feedUrl) return playEpisode(e);
                for (const auto& e : r->episodes)
                    if (!podcast::store().isPlayed(e.id())) return playEpisode(e);
                playEpisode(r->episodes.front());
            });
    }

    static void promptAddFeed() {
        auto* d = ui::Dialog::open(ctx().window, tr(L"RSS adresiyle podcast ekle"),
                                   tr(L"Podcastin RSS beslemesinin adresini yapıştır. Dizinde olmayan ve kendi sunucundaki podcastler de eklenebilir."), 520);
        if (!d) return;
        auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, L"https://…");
        auto submit = [d, box] {
            std::string url = toUtf8(box->text());
            while (!url.empty() && url.back() == ' ') url.pop_back();
            while (!url.empty() && url.front() == ' ') url.erase(url.begin());
            if (!podcast::isHttpUrl(url)) {
                box->setError(true);
                return;
            }
            d->close();
            toast(tr(L"Podcast ekleniyor…"));
            static Lifetime life;
            async(
                Priority::High, life.ref(), [url] { return podcast::Client::shared().feed(url, true, true); },
                [url](Result<Show> r) {
                    if (!r) {
                        ST_LOG_WARN("podcasts", "adding {} failed: {}", url, r.errorMessage());
                        toast(podcastError(r.errorMessage()), true);
                        return;
                    }
                    podcast::store().subscribe(*r, true, nowUnix());
                    toast(i18n::format(tr(L"\"{}\" eklendi"), {toWide(r->title)}));
                    openShow(r->feedUrl);
                });
        };
        box->onSubmit = [submit](const std::wstring&) { submit(); };
        box->onChange = [box](const std::wstring&) { box->setError(false); };
        d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
        d->addButton(tr(L"Ekle"), ButtonKind::Primary, submit, false);
        box->focus();
    }

    static void refreshSubscriptions(bool manual);

private:
    View view_ = View::Home;
    std::string arg_, appleId_;
    std::wstring query_, pending_;
    double pendingAt_ = 0;
    ui::Column *body_ = nullptr, *searchSec_ = nullptr, *progressSec_ = nullptr, *newSec_ = nullptr, *subsSec_ = nullptr,
               *chartsSec_ = nullptr;
    Show show_;
    ShowHeader* header_ = nullptr;
    EpisodeList* list_ = nullptr;
    ui::Label* empty_ = nullptr;
    std::wstring filter_;
    bool newestFirst_ = true;
    bool rebuildPosted_ = false, spotifyPosted_ = false;
    // Spotify (app/PodcastsSpotify): the show this page stands for, the home section, the import preview.
    std::string spotifyUri_;
    bool waitingForLink_ = false;   // a SpotifyShow page on its skeleton until the lookup has a verdict
    ui::Column* spotifySec_ = nullptr;
    std::vector<spotifyshows::ShowRow*> importRows_;
    ui::Label* importSummary_ = nullptr;
    Button* importButton_ = nullptr;
    spotifyshows::ListState importState_ = spotifyshows::ListState::NoSession;
    std::unordered_set<std::string> unchecked_;   // shows the user took out of the import
    std::shared_ptr<CancelSource> cts_ = std::make_shared<CancelSource>();
    Lifetime subs_, searchLife_, newLife_;
};

// ===================================================================================================
// Wiring: progress tracking, refresh of subscriptions

struct Tracking {
    std::string id;
    int64_t pos = 0, dur = 0;
    std::string seekId;        // dev (--play-episode ...@sec): seek this episode once it plays
    int64_t seekMs = -1;
};

Tracking& tracking() {
    static Tracking t;
    return t;
}

// The tracked episode ended (another item started, or the queue ran out): heard up to its last seconds = played.
void finishTracked() {
    auto& t = tracking();
    if (t.id.empty()) return;
    if (t.dur > 0 && t.pos >= t.dur - 4'000)
        if (const Episode* e = podcast::store().find(t.id)) podcast::store().setPosition(*e, t.dur, t.dur, nowUnix());
    t = {};
}

void sampleEpisode() {
    auto* p = ctx().player;
    const auto* cur = p ? p->current() : nullptr;
    auto& t = tracking();
    if (!cur || !catalog::isPodcastId(cur->id)) {
        finishTracked();
        return;
    }
    if (cur->id != t.id) {
        finishTracked();
        t.id = cur->id;
    }
    const auto status = p->status();
    if (status == player::Status::Idle) {   // the queue ran out after this episode
        finishTracked();
        t.id = cur->id;
        return;
    }
    if (status != player::Status::Playing && status != player::Status::Paused) return;
    if (t.seekMs >= 0 && t.seekId == cur->id && status == player::Status::Playing) {
        const int64_t to = std::exchange(t.seekMs, -1);   // before seek(): it notifies back into this hook
        ST_LOG_INFO("podcasts", "dev: seeking to {} ms", to);
        p->seek(to);
        return;
    }
    const int64_t pos = p->positionMs(), dur = p->durationMs();
    if (pos <= 0) return;
    t.pos = pos;
    t.dur = dur;
    if (const Episode* e = podcast::store().find(cur->id)) podcast::store().setPosition(*e, pos, dur, nowUnix());
}

struct Refresh {
    bool running = false;
    int64_t lastCheckMs = 0;
    int64_t startedMs = 0;
    Lifetime life;
};

Refresh& refreshState() {
    static Refresh r;
    return r;
}

constexpr int64_t kRefreshEverySec = 3 * 3600;

void refreshNext(std::vector<podcast::Subscription> due, size_t i, int added, bool manual) {
    auto& rs = refreshState();
    if (i >= due.size()) {
        rs.running = false;
        if (added > 0) toast(i18n::plural(L"{} yeni podcast bölümü", added), false, true);
        else if (manual) toast(tr(L"Abonelikler güncel"));
        repaint();
        return;
    }
    const auto sub = due[i];
    async(
        Priority::Low, rs.life.ref(), [sub] { return podcast::Client::shared().feed(sub.feedUrl, true, sub.manual); },
        [due = std::move(due), i, added, manual](Result<Show> r) mutable {
            int more = 0;
            if (r) more = podcast::store().refreshed(*r, nowUnix());
            else ST_LOG_WARN("podcasts", "refresh of {} failed: {}", due[i].feedUrl, r.errorMessage());
            refreshNext(std::move(due), i + 1, added + more, manual);
        });
}

} // namespace

void PodcastsPage::refreshSubscriptions(bool manual) {
    auto& rs = refreshState();
    if (rs.running) {
        if (manual) toast(tr(L"Abonelikler zaten yenileniyor"));
        return;
    }
    const int64_t now = nowUnix();
    std::vector<podcast::Subscription> due;
    for (const auto& s : podcast::store().subscriptions())
        if (manual || now - s.lastRefresh >= kRefreshEverySec) due.push_back(s);
    if (due.empty()) {
        if (manual) toast(tr(L"Henüz aboneliğin yok"));
        return;
    }
    rs.running = true;
    if (manual) toast(tr(L"Abonelikler yenileniyor…"));
    refreshNext(std::move(due), 0, 0, manual);
}

// ===================================================================================================
// Pieces other screens use (PodcastUi.h)

void showEpisodeMenu(const catalog::Track& episode, gfx::Point windowPos) {
    if (const Episode* e = podcast::store().find(episode.id)) ui::Menu::open(ctx().window, windowPos, episodeMenuItems(*e, true));
}

void openEpisodeShow(const catalog::Track& episode) {
    if (const Episode* e = podcast::store().find(episode.id)) openShow(e->feedUrl);
    else ctx().router->navigate({RouteKind::Podcasts});
}

void paintEpisodeNotes(Canvas& c, const Rect& r, const catalog::Track& episode) {
    const auto& col = colors();
    const Episode* e = podcast::store().find(episode.id);
    c.text(toUpperTr(tr(L"Bölüm notları")), type::monoLabel, {r.x, r.y, r.w, 14}, col.fgTertiary);
    auto& n = notesCache();
    const Rect body{r.x, r.y + 30, r.w, std::max(0.f, r.h - 30)};
    if (n.id != episode.id || n.width != r.w || n.height != body.h || n.known != (e != nullptr)) {
        n.id = episode.id;
        n.width = r.w;
        n.height = body.h;
        n.known = e != nullptr;
        // Whole lines only: long notes end with "…" on the last line that fits.
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = std::max(1, static_cast<int>(body.h / (type::bodyL.size * type::bodyL.lineHeight)));
        n.text = gfx::Text(e && !e->description.empty() ? toWide(e->description) : tr(L"Bu bölüm için not yok."), type::bodyL, wrap);
    }
    c.pushClip(body);
    c.text(n.text, body, col.fgSecondary);
    c.popClip();
}

int podcastNewEpisodeCount() { return podcast::store().newEpisodeCount(); }

void devPlayEpisode(const std::string& spec) {
    std::string url = spec;
    auto takeNumber = [&url](char mark) -> int64_t {
        const size_t at = url.rfind(mark);
        if (at == std::string::npos || at + 1 >= url.size()) return -1;
        const std::string digits = url.substr(at + 1);
        if (!std::all_of(digits.begin(), digits.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return -1;
        url.resize(at);
        return _atoi64(digits.c_str());
    };
    const int64_t seekSec = takeNumber('@');
    const int64_t index = std::max<int64_t>(0, takeNumber('#'));
    ST_LOG_INFO("podcasts", "dev: episode {} of {}", index, url);
    static Lifetime life;
    async(
        Priority::High, life.ref(), [url] { return podcast::Client::shared().feed(url, false, true); },
        [index, seekSec](Result<Show> r) {
            if (!r || r->episodes.empty()) {
                ST_LOG_WARN("podcasts", "dev: no episodes ({})", r ? std::string("empty feed") : r.errorMessage());
                toast(tr(L"Podcast yüklenemedi"), true);
                return;
            }
            const Episode& e = r->episodes[std::min<size_t>(static_cast<size_t>(index), r->episodes.size() - 1)];
            if (seekSec >= 0) {
                tracking().seekId = e.id();
                tracking().seekMs = seekSec * 1000;
            }
            playEpisode(e);
        });
}

// ===================================================================================================
// Startup

void initPodcasts() {
    auto& client = podcast::Client::shared();
    client.setUserAgent("ShadeTube/" + updater::currentVersion() + " (+https://github.com/shadesofdeath/ShadeTube)");
    client.setCacheDir(paths::cacheDir() / L"podcasts");
    podcast::store().load(paths::appData() / L"podcasts.json");
    refreshState().startedMs = steadyMs();
    auto* p = ctx().player;
    if (!p) return;
    // Episodes play from their feed's audio (redirects followed on a worker first); an episode the store no longer
    // knows fails instead of being matched on YouTube by its title.
    p->directStreamFor = [](const catalog::Track& t) -> std::optional<player::Player::DirectStream> {
        if (!catalog::isPodcastId(t.id)) return std::nullopt;
        player::Player::DirectStream d;
        const Episode* e = podcast::store().find(t.id);
        if (!e) {
            ST_LOG_WARN("podcasts", "unknown episode {}", t.id);
            d.resolve = [](const YoutubeExplode::CancellationToken&) -> player::Player::DirectStream {
                throw podcast::PodcastError("unknown episode");
            };
            return d;
        }
        d.url = e->url;
        d.mimeType = podcast::mimeFor(*e);
        d.resolve = [url = e->url, mime = d.mimeType, local = allowLocal(e->feedUrl)](const YoutubeExplode::CancellationToken& ct) {
            const auto m = podcast::Client::shared().resolveMedia(url, local, ct);
            player::Player::DirectStream out;
            out.url = m.url;
            Episode probe;
            probe.mimeType = m.mimeType;
            probe.url = m.url;
            out.mimeType = mime.empty() ? podcast::mimeFor(probe) : mime;
            out.contentLength = m.length;
            return out;
        };
        return d;
    };
    p->startPositionFor = [](const catalog::Track& t) -> int64_t {
        return catalog::isPodcastId(t.id) ? podcast::store().resumePosition(t.id) : 0;
    };
    // Downloaded episodes play from disk (while the file is still there).
    ctx().localFileResolvers.push_back([](const std::string& id) -> std::wstring {
        if (!catalog::isPodcastId(id)) return {};
        const std::string file = podcast::store().downloadedFile(id);
        if (file.empty()) return {};
        std::wstring path = toWide(file);
        std::error_code ec;
        return fs::exists(path, ec) ? path : std::wstring();
    });
    ctx().playerChangedHooks.push_back([] { sampleEpisode(); });
    ctx().trackChangedHooks.push_back([](const catalog::Track&) { sampleEpisode(); });
    // Every ~2 s: progress, a save at most every 30 s, the download queue and the periodic refresh of subscriptions.
    static int64_t lastSave = 0;
    ctx().housekeepingHooks.push_back([] {
        sampleEpisode();
        auto& st = podcast::store();
        if (st.dirty() && steadyMs() - lastSave >= 30'000) {
            lastSave = steadyMs();
            syncQueueEpisodes();
            st.save();
        }
        pumpDownloads();
        auto& rs = refreshState();
        if (!rs.running && steadyMs() - rs.startedMs >= 60'000 && steadyMs() - rs.lastCheckMs >= 10 * 60'000) {
            rs.lastCheckMs = steadyMs();
            PodcastsPage::refreshSubscriptions(false);
        }
    });
    ctx().persistHooks.push_back([] {
        sampleEpisode();
        syncQueueEpisodes();
        podcast::store().save();
        if (auto& q = dlq(); q.cancel) {   // the .part stays: downloading the episode again resumes it
            q.quitting = true;
            q.cancel->store(true);
        }
    });
    // The sidebar badge and the player bar follow subscription changes made on the pages.
    static auto owner = std::make_shared<char>();
    podcast::store().subscribe(owner, [] { Dispatcher::post([] { repaint(); }); });
}

std::unique_ptr<Page> makePodcastsPage(const std::string& id) { return std::make_unique<PodcastsPage>(id); }

} // namespace st::app
