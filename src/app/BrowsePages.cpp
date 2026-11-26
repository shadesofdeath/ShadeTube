// The Library page (Search: app/SearchPage.cpp, Artist: app/ArtistPage.cpp). Reads Spotify when logged in and the local
// library otherwise (see Source.h).
#include "app/Blacklist.h"
#include "app/BrowseWidgets.h"
#include "app/CollapsingHeader.h"
#include "app/DownloadSync.h"
#include "app/LibrarySort.h"
#include "app/LibraryView.h"
#include "app/LinkOpener.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SmartListsPage.h"
#include "app/PlaylistTransfer.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <memory>
#include <optional>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;

namespace {

// Row of tab buttons with the accent underline.
class Tabs : public ui::Widget {
public:
    explicit Tabs(std::vector<std::wstring> labels) {
        hitTestVisible = false;
        for (size_t i = 0; i < labels.size(); ++i) {
            auto* b = add<Button>(ButtonKind::Tab, labels[i]);
            b->onClick = [this, i] { select(static_cast<int>(i)); };
            buttons_.push_back(b);
        }
        buttons_[0]->setActive(true);
    }
    std::function<void(int)> onSelect;
    void select(int i, bool notify = true) {
        for (size_t k = 0; k < buttons_.size(); ++k) buttons_[k]->setActive(static_cast<int>(k) == i);
        if (notify && onSelect) onSelect(i);
    }
    float preferredHeight(float) override { return 37; }
    void layout() override {
        float x = 0;
        for (auto* b : buttons_) {
            const float w = b->naturalWidth();
            b->setRect({x, 0, w, 36});
            x += w + 24;
        }
    }
    void paint(Canvas& c) override {
        c.hline(rect().x, rect().right(), rect().bottom() - 1, colors().hairDefault);
        paintChildren(c);
    }

private:
    std::vector<Button*> buttons_;
};


// ===================================================================================================
// Library: playlists (with the Spotify folders), albums, artists and the play history. Grid or list, sorted and
// filtered by app/LibrarySort and painted by app/LibraryView (no widget per card / row).

using libview::Sort;
using libview::Tab;

// " · "-joined, empty parts skipped ("Çalma listesi · Ada · 12 şarkı").
std::wstring joinDot(std::initializer_list<std::wstring> parts) {
    std::wstring s;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!s.empty()) s += L" · ";
        s += p;
    }
    return s;
}

std::wstring albumKind(const Album& a) {
    return a.primaryType == "Single" ? std::wstring(tr(L"Tekli")) : a.primaryType == "EP" ? std::wstring(L"EP") : std::wstring(tr(L"Albüm"));
}

std::wstring sortLabel(Sort s, Tab t) {
    switch (s) {
    case Sort::Added: return tr(L"Son eklenenler");
    case Sort::Alpha: return tr(L"Alfabetik");
    case Sort::Creator: return t == Tab::Albums ? tr(L"Sanatçı") : tr(L"Oluşturan");
    default: return tr(L"Son çalınanlar");
    }
}

bool blank(const std::wstring& s) { return s.find_first_not_of(L" \t") == std::wstring::npos; }

class LibraryPage : public ScrollPage {
public:
    // `route` = the Library route's id: "folder:<id>" shows the playlists of one Spotify library folder; "albums",
    // "artists" and "history" open that tab.
    explicit LibraryPage(const std::string& route) {
        if (route.rfind("folder:", 0) == 0) folder_ = route.substr(7);
        else if (route == "albums") tab_ = 1;
        else if (route == "artists") tab_ = 2;
        else if (route == "history") tab_ = 3;
        else if (route.empty()) tab_ = g_lastTab;   // the sidebar / Back: the tab last shown (its scroll offset fits)
        if (folder_.empty()) g_lastTab = tab_;
        readDevPreset();
        rebuild();
        ctx().library.subscribe(libLife_.ref(), [this] { refreshItems(); });
        if (ctx().session) ctx().session->subscribe(libLife_.ref(), [this] { refreshItems(); });
        sync::subscribe(libLife_.ref(), [this] {   // the "downloaded" marks
            if (view_) view_->invalidate();
        });
        // SHADETUBE_LIBRARY_SCROLL=<DIPs> (screenshots of the lower rows): the first Library page opens scrolled down.
        static bool devScrolled = false;
        wchar_t buf[32];
        if (const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_LIBRARY_SCROLL", buf, 32); !devScrolled && n && n < 32) {
            devScrolled = true;
            restoreScroll(static_cast<float>(_wtof(buf)));
        }
    }

private:
    // What a cell opens and which menu it has (parallel to cells_ and the model's items).
    struct Act {
        Route open;
        enum class Menu { None, Playlist, Album } menu = Menu::None;
        sync::Target offline;   // its images come from the cell when the menu opens
    };

    Tab tabKind() const { return tab_ == 1 ? Tab::Albums : tab_ == 2 ? Tab::Artists : Tab::Playlists; }

    const spotify::PlaylistFolder* currentFolder() const {
        return !folder_.empty() && source::loggedIn() ? ctx().session->library().tree.folder(folder_) : nullptr;
    }

    // SHADETUBE_LIBRARY_VIEW="<grid|list>[:<recents|added|alpha|creator>[:<filter>]]" (screenshots): this page's view,
    // sort and filter, without touching the settings.
    void readDevPreset() {
        wchar_t buf[512] = {};
        if (!GetEnvironmentVariableW(L"SHADETUBE_LIBRARY_VIEW", buf, 512)) return;
        std::wstring spec = buf;
        auto take = [&spec] {
            const size_t colon = spec.find(L':');
            std::wstring part = spec.substr(0, colon);
            spec = colon == std::wstring::npos ? std::wstring() : spec.substr(colon + 1);
            return toUtf8(part);
        };
        devView_ = libview::parseView(take());
        devSort_ = libview::parseSort(take());
        filter_ = spec;
    }

    // The tab's view and sort (Settings::libraryView / librarySort).
    void loadPrefs() {
        const auto& s = Settings::get();
        const std::string id = libview::tabId(tabKind());
        const auto v = s.libraryView.find(id);
        const auto o = s.librarySort.find(id);
        viewMode_ = libview::viewFor(v != s.libraryView.end() ? v->second : std::string());
        sort_ = libview::sortFor(tabKind(), o != s.librarySort.end() ? o->second : std::string());
        if (devView_) viewMode_ = *devView_;
        if (devSort_) sort_ = libview::sortFor(tabKind(), libview::sortId(*devSort_));
    }

    void rebuild() {
        const float y = scroll_->scrollY();
        builtLoggedIn_ = source::loggedIn();
        loadPrefs();
        // "Played" as of now: what plays while the page is shown doesn't reorder it under the pointer (library
        // updates keep that order; the next visit or tab shows the new one).
        recents_ = libraryRecents();
        history_ = libraryPlayRecency();
        view_ = nullptr;
        table_ = nullptr;
        noMatch_ = nullptr;
        folderMeta_ = nullptr;
        auto* c = resetContent(24.f);
        // A folder that is gone (moved / deleted on Spotify, logged out): the whole library instead.
        const spotify::PlaylistFolder* folder = currentFolder();
        builtInFolder_ = folder != nullptr;
        if (folder) addFolderHeader(c, *folder);
        else addHeader(c);
        addToolbar(c);
        body_ = c->add<ui::Column>(24.f);
        buildBody();
        contentReady();
        if (y > 0) {
            scroll_->layout();
            scroll_->scrollTo(y, false);
        }
    }

    void addHeader(ui::Column* c) {
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"Kitaplık"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        // Logged in this creates the playlist on the user's Spotify account; logged out, a local one.
        Button* create = top->add<Button>(ButtonKind::Secondary, tr(L"Yeni çalma listesi"), "plus");
        create->onClick = [] {
            promptNewPlaylist([](const std::string& id) { ctx().router->navigate({RouteKind::Playlist, id}); });
        };
        // A playlist file or a YouTube playlist -> a local playlist (app/PlaylistTransfer).
        Button* import = top->add<Button>(ButtonKind::Ghost, tr(L"İçe aktar"), "list");
        import->onClick = [import] {
            const Rect br = import->toWindow(import->rect());
            transfer::showImportMenu({br.x, br.bottom() + 6});
        };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, create, import](ui::Box& b) {
            const float w = b.rect().w;
            title->setRect({0, 0, w * 0.6f, 64});
            const float cw = create->naturalWidth(), iw = import->naturalWidth();
            create->setRect({w - cw, 12, cw, 40});
            import->setRect({w - cw - 10 - iw, 12, iw, 40});
        };
        auto* tabs = c->add<Tabs>(
            std::vector<std::wstring>{tr(L"Çalma listeleri"), tr(L"Albümler"), tr(L"Sanatçılar"), tr(L"Geçmiş")});
        tabs->select(tab_, false);
        tabs->onSelect = [this](int i) {
            tab_ = g_lastTab = i;
            Dispatcher::post([this, ref = life_.ref()] {
                if (!ref.expired()) rebuild();
            });
        };
    }

    // One Spotify library folder: its name, the way back up and its playlist count.
    void addFolderHeader(ui::Column* c, const spotify::PlaylistFolder& f) {
        const auto& tree = ctx().session->library().tree;
        const auto* parent = f.parentId.empty() ? nullptr : tree.folder(f.parentId);
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(f.name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(f.name), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        // Up one level: the parent folder, else the whole library.
        const std::wstring upLabel = parent ? (parent->name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(parent->name))
                                            : std::wstring(tr(L"Kitaplık"));
        Button* up = top->add<Button>(ButtonKind::Secondary, upLabel, "arrow-back");
        const Route upRoute = parent ? Route{RouteKind::Library, "folder:" + parent->id} : Route{RouteKind::Library};
        up->onClick = [upRoute] { ctx().router->navigate(upRoute); };
        auto* meta = top->add<ui::Label>(toUpperTr(i18n::plural(L"{} çalma listesi", f.playlistCount)), type::monoLabel,
                                         ui::Tone::Tertiary);
        folderMeta_ = meta;
        top->onPreferredHeight = [](float) { return 84.f; };
        top->onLayout = [title, up, meta](ui::Box& b) {
            const float w = b.rect().w;
            const float uw = up->naturalWidth();
            meta->setRect({0, 0, w * 0.6f, 16});
            title->setRect({0, 20, w - uw - 24, 64});
            up->setRect({w - uw, 32, uw, 40});
        };
    }

    // One toolbar row: "Kitaplıkta ara" (the search pill) and how many items show on the left; "SIRALA <sort>" and the
    // Izgara / Liste toggle on the right (Geçmiş: the search and the count only).
    void addToolbar(ui::Column* c) {
        auto* bar = c->add<ui::Box>();
        auto* box = bar->add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Kitaplıkta ara"));
        filterBox_ = box;
        box->setText(filter_);
        box->onChange = [this](const std::wstring& s) { setFilter(s); };
        box->onEscape = [this, box] {
            if (!filter_.empty()) box->setText({}, true);
        };
        auto* count = bar->add<ui::Label>(L"", type::monoLabel, ui::Tone::Tertiary);
        countLabel_ = count;
        auto* sortTag = bar->add<ui::Label>(toUpperTr(tr(L"Sırala")), type::monoLabel, ui::Tone::Tertiary);
        auto* sort = bar->add<Button>(ButtonKind::Ghost, sortLabel(sort_, tabKind()));
        sort->setTooltip(tr(L"Sırala"));
        sort->onClick = [this, sort] { showSortMenu(sort); };
        auto* toggle = bar->add<ViewToggle>(viewMode_ == libview::View::List);
        toggle->onChange = [this](bool list) { setView(list ? libview::View::List : libview::View::Grid); };
        for (ui::Widget* w : std::initializer_list<ui::Widget*>{sortTag, sort, toggle}) w->setVisible(tab_ != 3);
        sortBtn_ = sort;
        viewToggle_ = toggle;
        bar->onPreferredHeight = [](float) { return 44.f; };
        bar->onLayout = [box, count, sortTag, sort, toggle](ui::Box& b) {
            const float w = b.rect().w;
            float x = w;
            if (toggle->visible()) {
                toggle->setRect({w - ViewToggle::kW, 4, ViewToggle::kW, 36});
                x = w - ViewToggle::kW - 16;
            }
            if (sort->visible()) {
                const float sw = sort->naturalWidth();
                sort->setRect({x - sw, 4, sw, 36});
                const float lw = sortTag->naturalWidth();
                sortTag->setRect({x - sw - 4 - lw, 4, lw, 36});
                x -= sw + 4 + lw + 24;
            }
            const float bw = std::clamp(x * 0.5f, 200.f, 300.f);
            box->setRect({0, 0, bw, 44});
            count->setRect({bw + 16, 4, std::max(0.f, x - bw - 16), 36});
        };
    }

    void syncViewButtons() {
        if (viewToggle_) viewToggle_->setList(viewMode_ == libview::View::List);
    }

    // "12 ÇALMA LİSTESİ" / "3 / 12" while the search narrows them.
    void syncCount(size_t shown, size_t total) {
        if (!countLabel_) return;
        std::wstring text;
        if (total > 0) {
            const auto n = static_cast<int64_t>(total);
            const std::wstring all = tab_ == 1   ? i18n::plural(L"{} albüm", n)
                                     : tab_ == 2 ? i18n::plural(L"{} sanatçı", n)
                                     : tab_ == 3 ? i18n::plural(L"{} şarkı", n)
                                                 : i18n::plural(L"{} öğe", n);
            text = shown == total ? toUpperTr(all) : i18n::number(static_cast<int64_t>(shown)) + L" / " + toUpperTr(all);
        }
        countLabel_->setText(text);
    }

    void showSortMenu(Button* anchor) {
        const Rect br = anchor->toWindow(anchor->rect());
        std::vector<ui::MenuItem> items;
        for (Sort s : libview::sortsFor(tabKind())) {
            ui::MenuItem it{sortLabel(s, tabKind()), "", L"", [this, s, ref = life_.ref()] {
                                if (!ref.expired()) setSort(s);
                            }};
            it.checked = s == sort_;
            items.push_back(std::move(it));
        }
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
    }

    void setSort(Sort s) {
        if (s == sort_) return;
        sort_ = s;
        auto& st = Settings::get();
        st.librarySort[libview::tabId(tabKind())] = libview::sortId(s);
        st.markDirty();
        if (sortBtn_) {
            sortBtn_->setLabel(sortLabel(s, tabKind()));
            sortBtn_->requestLayout();
        }
        applyOrder();
    }

    void setView(libview::View v) {
        if (v == viewMode_) return;
        viewMode_ = v;
        auto& st = Settings::get();
        st.libraryView[libview::tabId(tabKind())] = libview::viewId(v);
        st.markDirty();
        syncViewButtons();
        if (view_) view_->setMode(v == libview::View::List ? LibraryView::Mode::List : LibraryView::Mode::Grid);
        if (body_) body_->requestLayout();
        scroll_->contentChanged();
    }

    void setFilter(const std::wstring& s) {
        const bool wasBlank = blank(filter_);
        filter_ = s;
        if (tab_ == 3) {
            if (table_) {
                table_->setFilter(blank(s) ? std::wstring() : s);
                syncNoMatch();
                scroll_->contentChanged();
            }
            return;
        }
        // "Senin için listeler" shows only without a search: the body changes when that does.
        if (blank(filter_) != wasBlank && tab_ == 0 && !currentFolder()) {
            buildBody();
            return;
        }
        applyOrder();
    }

    // The tab's content: the history table, or the view of the items (+ "Senin için listeler" above playlists).
    void buildBody() {
        if (!body_) return;
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        body_->clearChildren();
        view_ = nullptr;
        table_ = nullptr;
        noMatch_ = nullptr;
        auto& lib = ctx().library;
        if (tab_ == 3) {
            if (lib.history().empty()) {
                emptyState(body_, "clock", tr(L"Henüz geçmiş yok"), tr(L"Dinlediğin şarkılar burada listelenir."));
                syncCount(0, 0);
            } else {
                TrackTable::Options o;
                o.showAdded = false;
                auto* table = body_->add<TrackTable>(o);
                table_ = table;
                std::vector<Track> tracks;
                for (const auto& h : lib.history()) tracks.push_back(h.track);
                table->setTracks(std::move(tracks));
                if (!blank(filter_)) table->setFilter(filter_);
                // Natural order (newest first); a filtered table plays what it shows.
                table->onPlay = [table](int i) { ctx().player->playContext(table->displayedTracks(), i, {"history", tr(L"Geçmiş")}); };
                noMatch_ = noResultsState(body_, tr(L"Aramanla eşleşen sonuç yok."));
                syncNoMatch();
            }
            body_->requestLayout();
            scroll_->contentChanged();
            return;
        }
        const auto* folder = currentFolder();
        if (tab_ == 0 && !folder && blank(filter_)) smart::addSection(body_, 1);   // "Senin için listeler" (hidden while there is none)
        const auto t1 = clock::now();
        makeModel();
        const double modelMs = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
        if (model_.size() == 0) {
            // An empty tab: what collects here and one way to start (Search).
            auto search = [] { ctx().router->navigate({RouteKind::Search}); };
            if (tab_ == 1)
                emptyState(body_, "album", tr(L"Kayıtlı albüm yok"), tr(L"Albüm sayfasındaki kalp ile albümleri buraya kaydet."),
                           tr(L"Albüm ara"), search, "search");
            else if (tab_ == 2)
                emptyState(body_, "artist", tr(L"Takip ettiğin sanatçı yok"), tr(L"Sanatçı sayfalarından takip etmeye başla."),
                           tr(L"Sanatçı ara"), search, "search");
            syncCount(0, 0);
        } else {
            view_ = body_->add<LibraryView>(viewMode_ == libview::View::List ? LibraryView::Mode::List : LibraryView::Mode::Grid,
                                           tab_ == 2 ? 160.f : 170.f);
            view_->onOpen = [this](uint32_t i) { openCell(i); };
            view_->onPlay = [this](uint32_t i) { playCell(i); };
            view_->onContext = [this](uint32_t i, gfx::Point wp) { menuFor(i, wp); };
            view_->metaFor = [this](uint32_t i) { return metaFor(i); };
            noMatch_ = noResultsState(body_, tr(L"Aramanla eşleşen sonuç yok."));
            noMatch_->setVisible(false);
            applyOrder();
        }
        if (folder && folder->playlistCount == 0)
            emptyState(body_, "folder", tr(L"Bu klasör boş"), tr(L"Spotify'da bu klasöre çalma listesi ekleyebilirsin."));
        body_->requestLayout();
        scroll_->contentChanged();
        ST_LOG_INFO("library", "{} tab: {} items, {} shown, built in {:.1f} ms (model {:.1f} ms, order {:.1f} ms)",
                    libview::tabId(tabKind()), model_.size(), view_ ? view_->count() : 0,
                    std::chrono::duration<double, std::milli>(clock::now() - t0).count(), modelMs, orderMs_);
    }

    // The library changed (a playlist added, a like, a Spotify reload): new items, same widgets (the search box keeps
    // its focus and the cursor its cell). A login / logout or the open folder going away rebuild the page.
    void refreshItems() {
        if (source::loggedIn() != builtLoggedIn_ || (!folder_.empty() && (currentFolder() != nullptr) != builtInFolder_)) {
            rebuild();
            return;
        }
        if (const auto* f = currentFolder(); f && folderMeta_)
            folderMeta_->setText(toUpperTr(i18n::plural(L"{} çalma listesi", f->playlistCount)));
        if (tab_ == 3 || !view_) {
            buildBody();
            return;
        }
        makeModel();
        if (model_.size() == 0) buildBody();
        else applyOrder();
    }

    void applyOrder() {
        if (!view_) return;
        const auto t0 = std::chrono::steady_clock::now();
        libview::Query q;
        q.sort = sort_;
        q.filter = filter_;
        if (const auto* f = currentFolder()) q.root = f->id;
        std::vector<uint32_t> order;
        for (const auto& r : model_.order(q)) order.push_back(r.item);
        const bool none = order.empty() && !blank(filter_);
        // The toolbar's count: the whole tab (in the open folder), and how many of them the search leaves.
        if (blank(filter_)) {
            unfiltered_ = order.size();
        } else if (unfiltered_ == SIZE_MAX) {
            libview::Query all = q;
            all.filter.clear();
            unfiltered_ = model_.order(all).size();
        }
        syncCount(order.size(), unfiltered_);
        view_->setCells(cells_, std::move(order));
        orderMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (noMatch_ && noMatch_->visible() != none) noMatch_->setVisible(none);
        if (body_) body_->requestLayout();
        scroll_->contentChanged();
    }

    // "No results" under a history table the search emptied (its height is just the header then).
    void syncNoMatch() {
        if (!noMatch_ || !table_) return;
        const bool none = !blank(filter_) && table_->preferredHeight(0) <= table_->headerHeight() + 8.5f;
        if (none != noMatch_->visible()) {
            noMatch_->setVisible(none);
            if (body_) body_->requestLayout();
        }
        syncCount(table_->displayedTracks().size(), table_->tracks().size());
    }

    // The tab's items and their cells. "Son çalınanlar" reads: the collection's own last play here (libraryRecents),
    // for albums / artists also the newest stream of one of their songs (libraryPlayRecency), Spotify's played / added
    // times (libraryV3) and, for local playlists, when they were created or last got a song.
    void makeModel() {
        std::vector<libview::Item> items;
        auto cells = std::make_shared<std::vector<LibraryCell>>();
        acts_.clear();
        auto add = [&](libview::Item it, LibraryCell cell, Act act) {
            items.push_back(std::move(it));
            cells->push_back(std::move(cell));
            acts_.push_back(std::move(act));
        };
        const auto& recents = recents_;
        const auto& history = history_;
        auto& lib = ctx().library;
        const spotify::LibrarySnapshot* snap = source::loggedIn() ? &ctx().session->library() : nullptr;
        bool spotifyTimes = false;   // does libraryV3 send its played times at all?
        if (snap)
            for (const auto& [uri, e] : snap->entries)
                if (e.playedAt > 0) {
                    spotifyTimes = true;
                    break;
                }
        auto entryOf = [snap](const std::string& key) -> const spotify::LibraryEntry* {
            if (!snap) return nullptr;
            const auto it = snap->entries.find(key);
            return it == snap->entries.end() ? nullptr : &it->second;
        };
        auto signals = [&](const std::string& context, const spotify::LibraryEntry* e) {
            libview::Signals s;
            s.spotifyItem = snap != nullptr;
            s.spotifyTimes = spotifyTimes;
            s.playedHere = recents.at(context);
            s.playedSpotify = e ? e->playedAt : 0;
            s.added = e ? e->addedAt : 0;
            return s;
        };
        const std::wstring playlistWord = tr(L"Çalma listesi");
        if (tab_ == 0 && snap) {
            for (const auto& p : snap->playlists) {
                const bool liked = p.id == spotify::Api::kLikedSongsUri;
                const std::string context = liked ? std::string("liked") : "playlist:" + p.id;
                const auto* e = entryOf(p.id);
                libview::Signals s = signals(context, e);
                if (!s.added) s.added = p.createdAt;   // created here before the next reload
                libview::Item it;
                it.id = p.id;
                it.name = toWide(p.name);
                it.creator = toWide(p.ownerName);
                if (const auto f = snap->tree.folderOf.find(p.id); f != snap->tree.folderOf.end()) it.folder = f->second;
                it.recent = libview::recentsTime(s);
                it.added = s.added;
                // Without libraryV3's pins Liked Songs stays on top, as it always did.
                it.pinned = snap->entries.empty() ? liked : (e && e->pinned);
                const std::wstring count = p.countKnown ? i18n::plural(L"{} şarkı", p.totalTracks) : std::wstring();
                LibraryCell cell;
                cell.key = p.id;
                cell.title = it.name;
                // No count from Spotify (e.g. "Your Episodes"): the owner instead of a misleading "0 şarkı".
                cell.gridSubtitle = !count.empty() ? count : !p.ownerName.empty() ? toWide(p.ownerName) : playlistWord;
                cell.listSubtitle = joinDot({playlistWord, toWide(p.ownerName), count});
                cell.images = p.images;
                cell.likedTile = liked;
                cell.pinned = it.pinned;
                cell.context = context;
                const Route open = liked ? Route{RouteKind::Liked} : Route{RouteKind::Playlist, p.id};
                cell.drop = open;
                const sync::Target offline = liked ? sync::likedTarget() : sync::Target{sync::Kind::Playlist, p.id, p.name, {}};
                cell.syncId = offline.id;
                add(std::move(it), std::move(cell), {open, Act::Menu::Playlist, offline});
            }
            for (const auto& f : snap->tree.folders) {
                const auto* e = entryOf("folder:" + f.id);
                libview::Item it;
                it.kind = libview::Item::Kind::Folder;
                it.id = f.id;
                it.name = f.name.empty() ? std::wstring(tr(L"Adsız klasör")) : toWide(f.name);
                it.folder = f.parentId;
                it.added = e ? e->addedAt : 0;
                it.pinned = e && e->pinned;
                LibraryCell cell;
                cell.key = "folder:" + f.id;
                cell.title = it.name;
                cell.gridSubtitle = i18n::plural(L"{} çalma listesi", f.playlistCount);
                cell.listSubtitle = joinDot({tr(L"Klasör"), cell.gridSubtitle});
                cell.placeholder = Placeholder::Folder;
                cell.playable = false;
                cell.pinned = it.pinned;
                add(std::move(it), std::move(cell), {Route{RouteKind::Library, "folder:" + f.id}, Act::Menu::None, {}});
            }
        } else if (tab_ == 0) {
            {   // Liked Songs: always on top
                libview::Item it;
                it.id = "liked";
                it.name = tr(L"Beğenilen Şarkılar");
                it.pinned = true;
                it.recent = recents.at("liked");
                LibraryCell cell;
                cell.key = "liked";
                cell.title = it.name;
                cell.gridSubtitle = i18n::plural(L"{} şarkı", static_cast<int64_t>(lib.liked().size()));
                cell.listSubtitle = joinDot({playlistWord, cell.gridSubtitle});
                cell.likedTile = true;
                cell.context = "liked";
                cell.drop = Route{RouteKind::Liked};
                const sync::Target offline = sync::likedTarget();
                cell.syncId = offline.id;
                add(std::move(it), std::move(cell), {Route{RouteKind::Liked}, Act::Menu::Playlist, offline});
            }
            for (const auto& p : lib.playlists()) {
                const std::string context = "playlist:" + p.id;
                libview::Signals s;
                s.playedHere = recents.at(context);
                s.added = p.createdAt;
                int64_t changed = 0;   // the newest song's added time: a list that just got songs is a recent one
                if (const auto* tracks = lib.playlistTracks(p.id))
                    for (const auto& t : *tracks) changed = std::max(changed, t.addedAt);
                libview::Item it;
                it.id = p.id;
                it.name = toWide(p.name);
                it.recent = std::max(libview::recentsTime(s), changed);
                it.added = p.createdAt;
                LibraryCell cell;
                cell.key = p.id;
                cell.title = it.name;
                cell.gridSubtitle = i18n::plural(L"{} şarkı", p.totalTracks);
                cell.listSubtitle = joinDot({playlistWord, cell.gridSubtitle});
                cell.images = p.images;
                cell.context = context;
                cell.drop = Route{RouteKind::Playlist, p.id};
                cell.syncId = p.id;
                add(std::move(it), std::move(cell),
                    {Route{RouteKind::Playlist, p.id}, Act::Menu::Playlist, sync::Target{sync::Kind::Playlist, p.id, p.name, {}}});
            }
        } else if (tab_ == 1) {
            for (const auto& a : snap ? snap->albums : lib.albums()) {
                const std::string artist = a.artists.empty() ? std::string() : a.artists[0].name;
                const auto* e = entryOf(a.id);
                libview::Signals s = signals("album:" + a.id, e);
                s.playedHere = std::max(s.playedHere, history.album(a.id, a.name, artist));
                libview::Item it;
                it.kind = libview::Item::Kind::Album;
                it.id = a.id;
                it.name = toWide(a.name);
                it.creator = toWide(artist);
                it.released = a.firstReleaseDate;
                it.recent = libview::recentsTime(s);
                it.added = s.added;
                it.pinned = e && e->pinned;
                LibraryCell cell;
                cell.key = a.id;
                cell.title = it.name;
                cell.gridSubtitle = albumSubtitle(a);
                cell.listSubtitle = joinDot({albumKind(a), it.creator, toWide(a.year())});
                cell.images = a.images;
                cell.placeholder = Placeholder::Album;
                cell.pinned = it.pinned;
                cell.context = "album:" + a.id;
                cell.syncId = a.id;
                add(std::move(it), std::move(cell),
                    {Route{RouteKind::Album, a.id}, Act::Menu::Album, sync::Target{sync::Kind::Album, a.id, a.name, {}}});
            }
        } else {
            for (const auto& a : snap ? snap->artists : lib.artists()) {
                const auto* e = entryOf(a.id);
                libview::Signals s = signals("artist:" + a.id, e);
                s.playedHere = std::max(s.playedHere, history.artist(a.name));
                libview::Item it;
                it.kind = libview::Item::Kind::Artist;
                it.id = a.id;
                it.name = toWide(a.name);
                it.recent = libview::recentsTime(s);
                it.added = s.added;
                it.pinned = e && e->pinned;
                LibraryCell cell;
                cell.key = a.id;
                cell.title = it.name;
                cell.gridSubtitle = cell.listSubtitle = artistSubtitle(a);
                cell.images = a.images;
                cell.placeholder = Placeholder::Artist;
                cell.circle = true;
                cell.playable = false;   // as the artist cards: no play button
                cell.pinned = it.pinned;
                cell.context = "artist:" + a.id;
                add(std::move(it), std::move(cell), {Route{RouteKind::Artist, a.id}, Act::Menu::None, {}});
            }
        }
        cells_ = std::move(cells);
        model_ = libview::Model(std::move(items), i18n::localeName());
        unfiltered_ = SIZE_MAX;
    }

    void openCell(uint32_t i) {
        if (i >= acts_.size()) return;
        const Route r = acts_[i].open;   // navigating replaces this page
        ctx().router->navigate(r);
    }

    // The play button: the collection's songs from the top, or pause / resume when it is what plays.
    void playCell(uint32_t i) {
        if (i >= acts_.size() || !cells_) return;
        auto* p = ctx().player;
        const std::string& context = (*cells_)[i].context;
        if (p && p->current() && !context.empty() && p->context().uri == context) {
            p->togglePause();
            return;
        }
        Route r = acts_[i].open;
        r.id += "#play";
        ctx().router->navigate(r);
    }

    void menuFor(uint32_t i, gfx::Point wp) {
        if (i >= acts_.size() || !cells_) return;
        const Act& a = acts_[i];
        sync::Target offline = a.offline;
        offline.images = (*cells_)[i].images;
        if (a.menu == Act::Menu::Playlist) showPlaylistCardMenu(a.open, offline, wp);
        else if (a.menu == Act::Menu::Album) showAlbumCardMenu(a.open.id, offline, wp);
    }

    // The list's right-hand column: when it was last played / added, for those sorts.
    std::wstring metaFor(uint32_t i) const {
        if (i >= model_.size()) return {};
        const auto& it = model_.items()[i];
        if (sort_ == Sort::Recents && it.recent > 0) return relativeTime(it.recent);
        if (sort_ == Sort::Added && it.added > 0) return trDate(it.added, true);
        return {};
    }

    static inline int g_lastTab = 0;   // the tab a plain Library route opens on
    int tab_ = 0;
    std::string folder_;   // Spotify library folder shown ("" = the whole library)
    std::wstring filter_;
    libview::View viewMode_ = libview::View::Grid;
    Sort sort_ = Sort::Recents;
    std::optional<libview::View> devView_;
    std::optional<Sort> devSort_;
    bool builtLoggedIn_ = false, builtInFolder_ = false;
    libview::Recents recents_;              // as of the last rebuild (see rebuild())
    libview::PlayRecency history_;
    libview::Model model_;
    std::shared_ptr<const std::vector<LibraryCell>> cells_;
    std::vector<Act> acts_;
    double orderMs_ = 0;
    size_t unfiltered_ = SIZE_MAX;        // items of the tab without the search (SIZE_MAX: not counted yet)
    ui::Column* body_ = nullptr;
    LibraryView* view_ = nullptr;
    TrackTable* table_ = nullptr;          // Geçmiş
    ui::Widget* noMatch_ = nullptr;        // the compact "no results" state
    ui::Label* folderMeta_ = nullptr;
    ui::TextBox* filterBox_ = nullptr;
    Button* sortBtn_ = nullptr;
    ViewToggle* viewToggle_ = nullptr;
    ui::Label* countLabel_ = nullptr;      // "12 ÇALMA LİSTESİ" in the toolbar
    Lifetime libLife_;
};

} // namespace

std::unique_ptr<Page> makeLibraryPage(const std::string& id) { return std::make_unique<LibraryPage>(id); }

} // namespace st::app
