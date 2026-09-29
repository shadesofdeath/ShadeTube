// İndirilenler: aktif indirmeler (ilerleme), kullanıcı klasörleri (koleksiyonlar) ve indirilen şarkılar.
#include "app/DownloadSync.h"
#include "app/Downloads.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "core/I18n.h"
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
#include <cmath>
#include <filesystem>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;

namespace {

// "1,5 GB" / "320 MB", with the UI language's decimal separator (the unit is a text too: French "Go").
std::wstring sizeStr(int64_t bytes) {
    if (bytes <= 0) return L"";
    const double mb = bytes / 1048576.0;
    wchar_t b[32];
    if (mb >= 1024) {
        swprintf(b, 32, L"%.1f", mb / 1024.0);
        std::wstring n = b;
        std::replace(n.begin(), n.end(), L'.', i18n::decimalSeparator());
        return i18n::format(tr(L"{} GB"), {n});
    }
    swprintf(b, 32, L"%.0f", mb);
    return i18n::format(tr(L"{} MB"), {b});
}

// Active/failed download row: cover, title/artist, an accent progress bar (or a "retry"/error state).
class DownloadRow : public ui::Widget {
public:
    explicit DownloadRow(const DownloadItem& item) : item_(item), title_(toWide(item.track.name), type::body),
                                                     sub_(toWide(item.track.artistLine()), type::caption) {
        focusable = true;
    }
    std::function<void()> onCancel;
    std::function<void()> onRetry;
    // Keyboard (Tab stop): the ring sits on the cancel / retry button; Enter / Space press it.
    bool activatable() const override { return true; }
    bool onActivate() override {
        auto action = item_.state == DlState::Failed ? onRetry : onCancel;
        if (!action) return false;
        action();
        return true;
    }
    Rect focusRect() const override { return {rect().right() - 36, rect().cy() - 16, 32, 32}; }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Circle; }
    float preferredHeight(float) override { return 64; }
    bool onMouseDown(const ui::MouseEvent& e) override { return btn_.contains(e.pos); }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (!btn_.contains(e.pos)) return;
        if (item_.state == DlState::Failed && onRetry) onRetry();
        else if (onCancel) onCancel();
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool h = btn_.contains(e.pos);
        if (h != btnHover_) { btnHover_ = h; invalidate(); }
    }
    void onMouseLeave() override { btnHover_ = false; invalidate(); }
    LPCWSTR cursor() const override { return btnHover_ ? IDC_HAND : IDC_ARROW; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        const Rect art{r.x, r.cy() - 24, 48, 48};
        drawArtwork(c, item_.track.album.images, art, 2, Placeholder::Album);
        const float x = art.right() + 14;
        const float rightW = 150;   // status label ("SIRADA", translations are longer) + the action button
        c.text(title_, {x, r.y + 12, r.w - x - rightW, 20}, col.fgPrimary, gfx::VAlign::Center);
        c.text(sub_, {x, r.y + 32, r.w - x - rightW, 16}, col.fgSecondary, gfx::VAlign::Center);
        // Progress bar / state.
        const float barY = r.bottom() - 16;
        const Rect track{x, barY, r.w - x - rightW, 4};
        if (item_.state == DlState::Downloading) {
            c.fillPill(track, gfx::isLightTheme() ? col.hairStrong : col.bgOverlay);   // light: bg.overlay is white
            Rect fill = track;
            fill.w = std::max(4.f, track.w * item_.progress);
            c.fillPill(fill, acc.base);
            const std::wstring pct = i18n::format(tr(L"%{}"), static_cast<long long>(item_.progress * 100));   // "%42"
            c.text(pct, type::monoMeta, {r.right() - rightW, r.y, rightW - 44, r.h}, acc.base, gfx::TextAlign::Trailing,
                   gfx::VAlign::Center);
        } else if (item_.state == DlState::Queued) {
            c.text(tr(L"SIRADA"), type::monoLabel, {r.right() - rightW, r.y, rightW - 44, r.h}, col.fgTertiary,
                   gfx::TextAlign::Trailing, gfx::VAlign::Center);
        } else if (item_.state == DlState::Failed) {
            c.text(tr(L"BAŞARISIZ"), type::monoLabel, {r.right() - rightW, r.y, rightW - 44, r.h}, col.error,
                   gfx::TextAlign::Trailing, gfx::VAlign::Center);
        }
        // Action button (cancel / retry) at far right.
        btn_ = {r.right() - 36, r.cy() - 16, 32, 32};
        if (btnHover_) c.fillCircle({btn_.x + 16, btn_.y + 16}, 16, col.overlayHover);
        c.icon(item_.state == DlState::Failed ? "refresh" : "close", {btn_.x + 8, btn_.y + 8, 16, 16}, col.fgSecondary);
        if (item_.state == DlState::Downloading)
            if (auto* w = window()) w->invalidateAfter(120);
    }

private:
    DownloadItem item_;
    gfx::Text title_, sub_;
    Rect btn_{};
    bool btnHover_ = false;
};

class DownloadsPage : public ScrollPage {
public:
    DownloadsPage() {
        ctx().downloads.subscribe(life_.ref(), [this] { scheduleRebuild(); });
        sync::subscribe(life_.ref(), [this] { scheduleRebuild(); });
        rebuild();
    }

private:
    void scheduleRebuild() {
        // Coalesce bursts of notifications into one rebuild next tick.
        if (rebuildQueued_) return;
        rebuildQueued_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            if (ref.expired()) return;
            rebuildQueued_ = false;
            rebuild();
        });
    }

    void openFolder() {
        const auto& s = Settings::get();
        std::filesystem::path p = s.downloadsDir.empty() ? paths::downloadsDir() : std::filesystem::path(toWide(s.downloadsDir));
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void rebuild() {
        auto& dm = ctx().downloads;
        auto* c = resetContent(24.f);

        // Header + actions.
        auto* top = c->add<ui::Box>();
        const std::wstring heading = collectionFilter_.empty() ? std::wstring(tr(L"İndirilenler")) : collectionName();
        auto* title = top->add<ui::Label>(heading, type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        auto* newBtn = top->add<Button>(ButtonKind::Secondary, tr(L"Yeni klasör"), "plus");
        auto* openBtn = top->add<Button>(ButtonKind::Ghost, tr(L"Klasörü aç"), "folder");
        Button* back =
            collectionFilter_.empty() ? nullptr : top->add<Button>(ButtonKind::Ghost, tr(L"Tümü"), "arrow-back");
        newBtn->onClick = [this] {
            promptNewCollection();
        };
        openBtn->onClick = [this] { openFolder(); };
        if (back) back->onClick = [this] {
            collectionFilter_.clear();
            rebuild();
        };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, newBtn, openBtn, back](ui::Box& b) {
            const float w = b.rect().w;
            title->setRect({0, 0, w * 0.5f, 64});
            float x = w;
            const float ow = openBtn->naturalWidth();
            x -= ow;
            openBtn->setRect({x, 12, ow, 40});
            const float nw = newBtn->naturalWidth();
            x -= nw + 10;
            newBtn->setRect({x, 12, nw, 40});
            if (back) {
                const float bw = back->naturalWidth();
                x -= bw + 10;
                back->setRect({x, 12, bw, 40});
            }
        };

        // Stats.
        const int done = static_cast<int>(std::count_if(dm.items().begin(), dm.items().end(),
                                                        [](const DownloadItem& i) { return i.state == DlState::Done; }));
        std::wstring stats = i18n::plural(L"{} şarkı", done);
        if (dm.totalBytes() > 0) stats += L" · " + sizeStr(dm.totalBytes());
        if (dm.activeCount() > 0) stats += L" · " + i18n::plural(L"{} sırada/iniyor", dm.activeCount());
        // SponsorBlock trimming over ALL downloads: the rest of this line (song count, size) is global too, even
        // inside a folder, so a folder-only total here would read as "N of these songs".
        int cutSegments = 0;
        int64_t cutMs = 0;
        for (const auto& i : dm.items()) {
            if (i.state != DlState::Done || i.cutSegments <= 0) continue;
            cutSegments += i.cutSegments;
            cutMs += i.cutMs;
        }
        if (cutSegments > 0) stats += L" · " + cutSummary(cutSegments, cutMs);
        c->add<ui::Label>(stats, type::secondary, ui::Tone::Tertiary);

        if (collectionFilter_.empty()) {
            // Active downloads. Sync downloads waiting or failed are summed up per collection below instead of one
            // row each (a synced list can queue thousands); the one downloading right now still shows here.
            std::vector<const DownloadItem*> active;
            for (const auto& i : dm.items()) {
                if (i.synced && i.state != DlState::Downloading) continue;
                if (i.state == DlState::Downloading || i.state == DlState::Queued || i.state == DlState::Failed) active.push_back(&i);
            }
            if (!active.empty()) {
                c->add<SectionHeader>(tr(L"İndiriliyor"), std::to_wstring(active.size()));
                for (const auto* i : active) {
                    auto* row = c->add<DownloadRow>(*i);
                    const std::string id = i->track.id;
                    // Cancelling a sync download takes the song out of its synced collection (else the next pass
                    // would queue it again).
                    if (i->synced) row->onCancel = [t = i->track] { sync::excludeTracks({t}); };
                    else row->onCancel = [id] { ctx().downloads.cancel(id); };
                    row->onRetry = [i2 = *i] { ctx().downloads.enqueue(i2.track); };
                }
            }

            // Collections kept offline (download sync).
            sync::buildSyncSection(c);

            // Collections (user folders).
            c->add<SectionHeader>(tr(L"Klasörlerin"), std::to_wstring(dm.collections().size()));
            if (dm.collections().empty()) {
                c->add<ui::Label>(tr(L"Henüz klasör yok. İndirdiklerini gruplamak için \"Yeni klasör\" oluştur."),
                                  type::secondary, ui::Tone::Tertiary);
            } else {
                auto* grid = addCardRow(c, 150, 1);
                c->setSpacingBefore(grid, 12);
                for (const auto& col2 : dm.collections()) {
                    std::vector<Image> imgs;
                    const auto tracks = dm.collectionTracks(col2.id);
                    for (const auto& t : tracks)
                        if (!t.album.images.empty()) { imgs = t.album.images; break; }
                    auto* card = grid->add<MediaCard>(toWide(col2.name),
                                                      i18n::plural(L"{} şarkı", col2.trackIds.size()), imgs,
                                                      MediaCard::Shape::Square, Placeholder::Playlist);
                    const std::string cid = col2.id;
                    card->onOpen = [this, cid] {
                        collectionFilter_ = cid;
                        rebuild();
                    };
                    card->onContext = [this, cid](gfx::Point wp) { collectionMenu(cid, wp); };
                }
            }
        }

        // Track table (all downloaded, or the selected collection).
        std::vector<Track> tracks;
        if (collectionFilter_.empty()) {
            for (const auto& i : dm.items())
                if (i.state == DlState::Done) tracks.push_back(i.track);
        } else {
            tracks = dm.collectionTracks(collectionFilter_);
        }
        if (!tracks.empty()) {
            if (collectionFilter_.empty()) c->add<SectionHeader>(tr(L"Şarkılar"), std::to_wstring(tracks.size()));
            TrackTable::Options o;
            o.showAdded = false;
            auto* table = c->add<TrackTable>(o);
            table->setTracks(tracks);
            table->onPlay = [tracks](int i) {
                ctx().player->playContext(tracks, i, {"downloads", tr(L"İndirilenler")});
            };
        } else if (collectionFilter_.empty() && dm.activeCount() == 0) {
            c->add<MessagePanel>("download", tr(L"Henüz indirilen şarkı yok"),
                                 tr(L"Bir şarkıya sağ tıklayıp \"İndir\" de; MP3 olarak buraya insin."));
        }
        contentReady();
    }

    std::wstring collectionName() const {
        const auto* c = ctx().downloads.collection(collectionFilter_);
        return c ? toWide(c->name) : std::wstring(tr(L"Klasör"));
    }

    void promptNewCollection() {
        auto* d =
            ui::Dialog::open(ctx().window, tr(L"Yeni klasör"), tr(L"İndirdiklerini gruplamak için bir isim ver."));
        if (!d) return;
        auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Klasör adı"));
        auto submit = [box] { ctx().downloads.createCollection(box->text()); };
        box->onSubmit = [d, submit](const std::wstring&) {
            submit();
            d->close();
        };
        d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
        d->addButton(tr(L"Oluştur"), ButtonKind::Primary, submit);
        box->focus();
    }

    void collectionMenu(const std::string& cid, gfx::Point wp) {
        std::vector<ui::MenuItem> items;
        items.push_back({tr(L"Aç"), "arrow-up-right", L"", [this, cid] {
                             collectionFilter_ = cid;
                             rebuild();
                         }});
        items.push_back({tr(L"Yeniden adlandır"), "edit", L"", [cid] {
                             auto* d = ui::Dialog::open(ctx().window, tr(L"Yeniden adlandır"));
                             if (!d) return;
                             auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Klasör adı"));
                             if (const auto* c = ctx().downloads.collection(cid)) box->setText(toWide(c->name));
                             auto apply = [box, cid] { ctx().downloads.renameCollection(cid, box->text()); };
                             box->onSubmit = [d, apply](const std::wstring&) { apply(); d->close(); };
                             d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
                             d->addButton(tr(L"Kaydet"), ButtonKind::Primary, apply);
                             box->focus();
                         }});
        items.push_back(ui::MenuItem::sep());
        ui::MenuItem del{tr(L"Klasörü sil"), "trash", L"", [cid] {
                             ctx().downloads.deleteCollection(cid);
                         }};
        del.destructive = true;
        items.push_back(std::move(del));
        ui::Menu::open(ctx().window, wp, std::move(items));
    }

    std::string collectionFilter_;
    bool rebuildQueued_ = false;
};

} // namespace

std::unique_ptr<Page> makeDownloadsPage() { return std::make_unique<DownloadsPage>(); }

} // namespace st::app
