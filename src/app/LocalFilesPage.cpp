// Yerel dosyalar: the user's own music folders (app/LocalLibrary) — the page, the Ayarlar › YEREL MÜZİK rows and
// the wiring (player resolver, startup scan, exit).
//
// Views (tabs): Şarkılar, Albümler, Sanatçılar and Klasörler, which browses the listed folders as trees (breadcrumb,
// subfolder cards, the folder's own songs). The view and the open folder are remembered (Settings::localView /
// localFolder) and are part of the route ({LocalFiles, "songs" | "albums" | "artists" | "folders" | "folder:<path>"};
// "" = the remembered one), so Back / Forward walk the folders.
#include "app/AppContext.h"
#include "app/LocalLibrary.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PlaylistTransfer.h"
#include "app/SettingsWidgets.h"
#include "app/TagEditor.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <unordered_map>

namespace st::app {

using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using Microsoft::WRL::ComPtr;
using namespace catalog;
namespace type = gfx::type;

namespace {

constexpr size_t kMaxCards = 300;   // album / artist grids: beyond this, narrow it down with the search box

// Folder picker (IFileOpenDialog, FOS_PICKFOLDERS) owned by the main window. Modal: call it posted (outside a widget
// event). Adds the folder (+ rescan); false when cancelled or already listed.
bool pickLocalFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(tr(L"Müzik klasörü seç"));
    dlg->SetOkButtonLabel(tr(L"Klasörü ekle"));
    ComPtr<IShellItem> music;   // first time: start in Müzik (later Windows remembers the last folder)
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Music, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&music))))
        dlg->SetDefaultFolder(music.Get());
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return false;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
    const std::wstring folder = path;
    CoTaskMemFree(path);
    if (!LocalLibrary::get().addFolder(folder)) {
        toast(tr(L"Bu klasör zaten taranıyor"));
        return false;
    }
    toast(tr(L"Klasör eklendi, şarkılar taranıyor…"));
    return true;
}

void addFolderPosted(std::function<void()> then = {}) {
    Dispatcher::post([then = std::move(then)] {
        if (pickLocalFolder() && then) then();
    });
}

std::wstring songCount(int64_t n) { return i18n::plural(L"{} şarkı", n); }   // "1.234 şarkı": plural() groups digits

// "1.234 şarkı · 3 sa 12 dk"
std::wstring countLine(size_t n, int64_t ms) {
    std::wstring s = songCount(static_cast<int64_t>(n));
    if (ms >= 60'000) s += L" · " + totalDuration(ms);   // (not "0 dk")
    return s;
}

// "Taranıyor… 120 / 500" / "Son tarama 3 saat önce · 1 klasöre ulaşılamıyor" / "Henüz taranmadı"
std::wstring scanLine() {
    const auto& lib = LocalLibrary::get();
    if (lib.scanning()) {
        if (lib.scanTotal() <= 0) return tr(L"Taranıyor…");
        return i18n::format(tr(L"Taranıyor… {} / {}"), {thousands(lib.scanDone()), thousands(lib.scanTotal())});
    }
    std::wstring s = lib.lastScanAt() > 0 ? i18n::format(tr(L"Son tarama {}"), {relativeTime(lib.lastScanAt())})
                                          : std::wstring(tr(L"Henüz taranmadı"));
    const auto& st = lib.lastStats();
    constexpr auto maxFiles = static_cast<int64_t>(local::kMaxFiles);
    if (lib.capped()) s += L" · " + i18n::plural(L"ilk {} dosya listeleniyor (sınır)", maxFiles);
    if (!st.missingRoots.empty())
        s += L" · " + i18n::plural(L"{} klasöre ulaşılamıyor", static_cast<long long>(st.missingRoots.size()));
    if (st.failed > 0) s += L" · " + i18n::plural(L"{} dosya okunamadı", st.failed);
    return s;
}

// Row of tab buttons with the hairline underneath (like Kitaplık).
class Tabs : public ui::Widget {
public:
    Tabs(std::vector<std::wstring> labels, int selected) {
        hitTestVisible = false;
        for (size_t i = 0; i < labels.size(); ++i) {
            auto* b = add<Button>(ButtonKind::Tab, labels[i]);
            b->setActive(static_cast<int>(i) == selected);
            b->onClick = [this, i] {
                for (size_t k = 0; k < buttons_.size(); ++k) buttons_[k]->setActive(k == i);
                if (onSelect) onSelect(static_cast<int>(i));
            };
            buttons_.push_back(b);
        }
    }
    std::function<void(int)> onSelect;
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

// ---- Klasörler ----------------------------------------------------------------------------------------------------

// The songs of a folder and its subfolders, in tree order ("" = every listed folder). Resolved when asked (cards keep
// a path, not tree indices, so they stay right while the tree is rebuilt).
std::vector<Track> tracksUnder(const std::wstring& path) {
    auto& lib = LocalLibrary::get();
    const auto& all = lib.tracks();
    std::vector<Track> out;
    if (lib.folderTreeReady()) {
        const auto tree = lib.folderTree();
        std::vector<int> nodes;
        if (path.empty()) nodes = tree->roots;
        else if (const int n = tree->find(path); n >= 0) nodes.push_back(n);
        for (int n : nodes)
            for (int i : tree->subtree(n))
                if (i < static_cast<int>(all.size())) out.push_back(all[static_cast<size_t>(i)]);
        return out;
    }
    const auto& entries = lib.entries();
    for (size_t i = 0; i < entries.size() && i < all.size(); ++i)
        if (path.empty() || local::isUnder(entries[i].path, path)) out.push_back(all[i]);
    return out;
}

player::PlayContext folderContext(const std::wstring& path, const std::wstring& name) {
    return {"local:folder:" + toUtf8(toLowerTr(path)), name.empty() ? std::wstring(tr(L"Yerel dosyalar")) : name};
}

// A folder's menu (a card's right click / the "⋯" button): the actions the song menus have, for the whole folder.
void showFolderMenu(const std::wstring& path, const std::wstring& name, gfx::Point at) {
    auto tracks = std::make_shared<const std::vector<Track>>(tracksUnder(path));
    if (tracks->empty()) return;
    std::vector<ui::MenuItem> items;
    items.push_back({tr(L"Karıştır"), "shuffle", L"", [tracks, context = folderContext(path, name)] {
                         ctx().player->setShuffle(true);
                         ctx().player->playContext(*tracks, -1, context);
                     }});
    items.push_back({tr(L"Sonra çal"), "queue", L"", [tracks] {
                         for (auto it = tracks->rbegin(); it != tracks->rend(); ++it) ctx().player->playNext(*it);
                         toast(tr(L"Sıradaki olarak eklendi"));
                     }});
    items.push_back({tr(L"Tümünü sıraya ekle"), "plus", L"", [tracks] {
                         ctx().player->enqueue(*tracks);
                         toast(tr(L"Sıraya eklendi"));
                     }});
    items.push_back({tr(L"Çalma listesine ekle…"), "playlist", L"", [tracks, at] {
                         Dispatcher::post([tracks, at] { showAddToPlaylistMenu(*tracks, at); });
                     }});
    items.push_back(ui::MenuItem::sep());
    if (auto edit = tagedit::menuItem(*tracks)) items.push_back(std::move(*edit));
    items.push_back({tr(L"Dışa aktar…"), "share", L"", [tracks, name] { transfer::exportTracks(toUtf8(name), *tracks); }});
    if (!path.empty())
        items.push_back({tr(L"Klasörü aç"), "folder", L"", [path] {
                             const std::wstring arg = L"\"" + path + L"\"";
                             ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                         }});
    ui::Menu::open(ctx().window, at, std::move(items));
}

// The open folder: the way up as a breadcrumb ("Tüm klasörler › Müzik › Rock ›", buttons), then its name with a mono
// meta line, in the section header style.
class FolderHead : public ui::Widget {
public:
    struct Step {
        std::wstring label;
        std::wstring path;   // "" = the top level
    };
    FolderHead(const std::vector<Step>& up, std::wstring name, std::wstring meta, std::function<void(const std::wstring&)> open)
        : name_(std::move(name), type::title), meta_(toUpperTr(meta), type::monoLabel) {
        hitTestVisible = false;
        for (const auto& s : up) {
            auto* b = add<Button>(ButtonKind::Ghost, s.label);
            b->onClick = [open, path = s.path] {
                const auto go = open;   // navigates: this widget goes away
                go(path);
            };
            buttons_.push_back(b);
        }
    }
    float preferredHeight(float) override { return crumbsH() + 40; }
    void layout() override {
        float x = 0;   // (like the Albümler / Sanatçılar way back: a ghost button at the left edge)
        chevrons_.clear();
        for (auto* b : buttons_) {
            const float w = std::min(b->naturalWidth(), 240.f);
            b->setRect({x, 0, w, 32});
            x += w;
            chevrons_.push_back(x - 4);
            x += 8;
        }
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        for (float cx : chevrons_) c.icon("chevron-right", {r.x + cx, r.y + 8, 16, 16}, colors().fgTertiary);
        const float y = r.y + crumbsH();
        const float tw = std::min(std::ceil(name_.measure().w), r.w * 0.7f);
        c.text(name_, {r.x, y, tw + 1, 36}, colors().fgPrimary, gfx::VAlign::Center);
        if (!meta_.empty()) c.text(meta_, {r.x + tw + 14, y + 4, std::max(0.f, r.w - tw - 14), 36}, colors().fgTertiary, gfx::VAlign::Center);
        paintChildren(c);
    }

private:
    float crumbsH() const { return buttons_.empty() ? 0.f : 36.f; }
    gfx::Text name_, meta_;
    std::vector<Button*> buttons_;
    std::vector<float> chevrons_;
};

// Album / artist group of the library (indices into LocalLibrary::tracks()).
struct Group {
    std::string key;   // stable identity (survives rescans): lower-cased album + album artist, or artist name
    std::wstring title, subtitle;
    std::vector<Image> images;
    std::vector<int> tracks;
};

class LocalFilesPage : public ScrollPage {
public:
    explicit LocalFilesPage(const std::string& routeId) {
        const auto& s = Settings::get();
        const std::string view = routeId.empty() ? s.localView : routeId.rfind("folder:", 0) == 0 ? "folders" : routeId;
        tab_ = view == "albums" ? Albums : view == "artists" ? Artists : view == "folders" ? Folders : Songs;
        if (routeId.rfind("folder:", 0) == 0) folder_ = toWide(routeId.substr(7));
        else if (routeId.empty() && tab_ == Folders) folder_ = toWide(s.localFolder);
        remember();
        if (ctx().router && ctx().router->current().kind == RouteKind::LocalFiles) ctx().router->retarget(route());
        LocalLibrary::get().subscribe(libLife_.ref(), [this] { onLibraryChanged(); });
        build();
    }

    void paint(Canvas& c) override {
        if (play_) {
            const auto* p = ctx().player;
            play_->setPlaying(p && p->isPlaying() && p->context().uri == currentContext().uri);
        }
        ScrollPage::paint(c);
    }

private:
    enum Tab { Songs, Albums, Artists, Folders };

    // The route naming what the page shows (Back / Forward come back to it).
    Route route() const {
        switch (tab_) {
        case Albums: return {RouteKind::LocalFiles, "albums"};
        case Artists: return {RouteKind::LocalFiles, "artists"};
        case Folders: return {RouteKind::LocalFiles, folder_.empty() ? std::string("folders") : "folder:" + toUtf8(folder_)};
        default: return {RouteKind::LocalFiles, "songs"};
        }
    }
    // The next visit opens the same view (and folder).
    void remember() const {
        auto& s = Settings::get();
        const std::string view = tab_ == Albums ? "albums" : tab_ == Artists ? "artists" : tab_ == Folders ? "folders" : "songs";
        if (s.localView != view || (tab_ == Folders && s.localFolder != toUtf8(folder_))) {
            s.localView = view;
            if (tab_ == Folders) s.localFolder = toUtf8(folder_);
            s.markDirty();
        }
    }

    // Structural state: a change rebuilds the page; anything else (scan progress) only refreshes the labels.
    struct Shape {
        uint64_t generation;
        bool loaded, noFolders, emptyScanning;
        bool operator==(const Shape&) const = default;
    };
    static Shape shape() {
        const auto& lib = LocalLibrary::get();
        return {lib.generation(), lib.loaded(), Settings::get().localFolders.empty(), lib.tracks().empty() && lib.scanning()};
    }

    void onLibraryChanged() {
        auto& lib = LocalLibrary::get();
        const Shape now = shape();
        if (now != built_) {
            // Klasörler showing a tree: keep it on screen until the new generation's tree is ready (no flicker after a
            // rescan or a tag edit; its cards resolve their songs when clicked, so nothing goes stale).
            if (tab_ == Folders && showsTree_ && now.loaded == built_.loaded && now.noFolders == built_.noFolders &&
                !now.emptyScanning && !lib.tracks().empty() && !lib.folderTreeReady()) {
                lib.folderTree();   // builds it; its arrival notifies again
                refreshStatus();
                return;
            }
            pendingScroll_ = scroll_->scrollY();   // restored by contentReady()
            build();
            return;
        }
        if (tab_ == Folders && waitingForTree_ && lib.folderTreeReady()) {
            buildBody();
            contentReady();
            return;
        }
        refreshStatus();
    }

    void refreshStatus() {
        const auto& lib = LocalLibrary::get();
        if (status_) status_->setText(statusText());
        if (rescan_) {
            rescan_->setLabel(lib.scanning() ? tr(L"Taranıyor…") : tr(L"Yeniden tara"));
            rescan_->setEnabled(!lib.scanning());
            if (auto* top = rescan_->parent()) top->requestLayout();
        }
        invalidate();
    }

    std::wstring statusText() const {
        const auto& lib = LocalLibrary::get();
        std::wstring s = countLine(lib.tracks().size(), lib.totalMs());
        const size_t folders = Settings::get().localFolders.size();
        if (folders > 0) s += L" · " + i18n::plural(L"{} klasör", static_cast<long long>(folders));
        return s + L" · " + scanLine();
    }

    void build() {
        const auto& lib = LocalLibrary::get();
        built_ = shape();
        play_ = nullptr;
        table_ = nullptr;
        body_ = nullptr;
        more_ = nullptr;
        showsTree_ = waitingForTree_ = false;
        auto* c = resetContent(20.f);

        // Title + actions.
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"Yerel dosyalar"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        auto* add = top->add<Button>(ButtonKind::Secondary, tr(L"Klasör ekle"), "plus");
        add->setTooltip(tr(L"Taranacak bir müzik klasörü seç"));
        add->onClick = [] { addFolderPosted(); };
        rescan_ = top->add<Button>(ButtonKind::Ghost, tr(L"Yeniden tara"), "refresh");
        rescan_->setTooltip(tr(L"Klasörlerdeki yeni, değişen ve silinen dosyaları bul"));
        rescan_->onClick = [] { LocalLibrary::get().rescan(); };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, add, r = rescan_](ui::Box& b) {
            const float w = b.rect().w;
            float x = w - add->naturalWidth();
            add->setRect({x, 12, add->naturalWidth(), 40});
            x -= r->naturalWidth() + 10;
            r->setRect({x, 12, r->naturalWidth(), 40});
            title->setRect({0, 0, std::max(0.f, x - 16), 64});
        };
        status_ = c->add<ui::Label>(L"", type::secondary, ui::Tone::Tertiary);
        refreshStatus();

        // Empty states.
        if (Settings::get().localFolders.empty() && lib.tracks().empty()) {
            rescan_->setVisible(false);
            status_->setVisible(false);
            emptyPanel(c->add<MessagePanel>("folder", tr(L"Bilgisayarındaki müziği ekle"),
                                             tr(L"Müzik klasörlerini seç; şarkılar etiketleri ve kapaklarıyla burada "
                                                L"listelenir, internet olmadan çalar. Dosyaların yalnızca sen etiketlerini "
                                                L"düzenlediğinde değiştirilir."),
                                             tr(L"Klasör ekle"), [] { addFolderPosted(); }),
                       "plus");
            contentReady();
            return;
        }
        if (lib.tracks().empty()) {
            if (lib.scanning() || !lib.loaded())
                c->add<MessagePanel>("refresh", tr(L"Klasörler taranıyor"),
                                     tr(L"Bu biraz sürebilir; bitince şarkıların burada listelenir."));
            else
                emptyPanel(c->add<MessagePanel>("music-note", tr(L"Şarkı bulunamadı"),
                                                tr(L"Eklenen klasörlerde çalınabilir ses dosyası yok. Desteklenen "
                                                   L"biçimler: MP3, M4A, AAC, FLAC, WAV ve WMA."),
                                                tr(L"Yeniden tara"), [] { LocalLibrary::get().rescan(); }),
                           "refresh");
            contentReady();
            return;
        }

        // Play / shuffle + search.
        auto* bar = c->add<ui::Box>();
        play_ = bar->add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        play_->setTooltip(tr(L"Çal"));
        play_->onClick = [this] { playAll(false); };
        auto* shuffle = bar->add<Button>(ButtonKind::Secondary, tr(L"Karıştır"), "shuffle");
        shuffle->onClick = [this] { playAll(true); };
        more_ = bar->add<Button>(ButtonKind::IconOutline, L"", "more");   // Klasörler: the open folder's menu
        more_->setTooltip(tr(L"Diğer seçenekler"));
        more_->setVisible(tab_ == Folders);
        more_->onClick = [this] {
            const Rect br = more_->toWindow(more_->rect());
            showFolderMenu(shownFolder_, shownName_, {br.x, br.bottom() + 6});
        };
        filterBox_ = bar->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Yerel dosyalarda ara"));
        filterBox_->setText(filter_);
        filterBox_->onChange = [this](const std::wstring& s) {
            filter_ = s;
            if (table_ && tab_ != Folders) {   // (Klasörler filters its cards too)
                table_->setFilter(s);
                syncNoMatch();
                scroll_->contentChanged();
            } else {
                buildBody();
            }
        };
        filterBox_->onEscape = [this] {
            if (!filter_.empty()) filterBox_->setText({}, true);
        };
        bar->onPreferredHeight = [](float) { return 56.f; };
        bar->onLayout = [p = play_, shuffle, more = more_, box = filterBox_](ui::Box& b) {
            const float w = b.rect().w;
            p->setRect({0, 0, 56, 56});
            const float sw = shuffle->naturalWidth();
            shuffle->setRect({56 + 16, 8, sw, 40});
            float used = 56 + 16 + sw;
            if (more->visible()) {
                more->setRect({used + 14, 8, 40, 40});
                used += 14 + 40;
            }
            const float bw = std::min(280.f, std::max(160.f, w - (used + 24)));
            box->setRect({w - bw, 6, bw, 44});
        };

        auto* tabs = c->add<Tabs>(std::vector<std::wstring>{tr(L"Şarkılar"), tr(L"Albümler"), tr(L"Sanatçılar"), tr(L"Klasörler")},
                                  tab_);
        tabs->onSelect = [this, bar](int i) {
            closeGroup();
            tab_ = static_cast<Tab>(i);
            remember();
            if (ctx().router) ctx().router->retarget(route());
            more_->setVisible(tab_ == Folders);
            bar->requestLayout();
            buildBody();
        };
        body_ = c->add<ui::Column>(16.f);
        buildBody();
        contentReady();
    }

    // MessagePanel's action button carries a "refresh" icon; give it the right one.
    static void emptyPanel(MessagePanel* panel, const char* icon) {
        for (const auto& child : panel->children())
            if (auto* b = dynamic_cast<Button*>(child.get())) b->setIcon(icon);
    }

    // The tab's content (table / grid / a drilled-down album or artist). The header, search box and tabs stay.
    void buildBody() {
        if (!body_) return;
        body_->clearChildren();
        table_ = nullptr;
        noMatch_ = nullptr;
        gridTracks_.clear();
        waitingForTree_ = showsTree_ = false;
        const auto& tracks = LocalLibrary::get().tracks();
        if (tab_ == Folders) {
            buildFolders();
        } else if (tab_ == Songs) {
            addTable(tracks, false);
        } else {
            const auto groups = tab_ == Albums ? albumGroups() : artistGroups();
            const auto open = drillKey_.empty() ? groups.end()
                                                : std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.key == drillKey_; });
            if (!drillKey_.empty() && open == groups.end()) closeGroup();   // gone after a rescan: back to the grid
            if (open != groups.end()) {
                const Group& g = *open;
                drillTitle_ = g.title;
                auto* head = body_->add<ui::Box>();
                const wchar_t* backLabel = tab_ == Albums ? tr(L"Tüm albümler") : tr(L"Tüm sanatçılar");
                auto* back = head->add<Button>(ButtonKind::Ghost, backLabel, "arrow-back");
                back->onClick = [this] {
                    closeGroup();
                    Dispatcher::post([this, ref = libLife_.ref()] {
                        if (!ref.expired()) buildBody();
                    });
                };
                std::vector<Track> list;
                int64_t ms = 0;
                for (int i : g.tracks) {
                    list.push_back(tracks[i]);
                    ms += tracks[i].durationMs;
                }
                auto* name = head->add<ui::Label>(g.title, type::sectionTitle);
                auto* meta = head->add<ui::Label>(countLine(list.size(), ms), type::secondary, ui::Tone::Tertiary);
                head->onPreferredHeight = [](float) { return 40.f; };
                head->onLayout = [back, name, meta](ui::Box& b) {
                    const float bw = back->naturalWidth();
                    back->setRect({0, 2, bw, 36});
                    const float nw = std::min(name->naturalWidth(), std::max(0.f, b.rect().w - bw - 200));
                    name->setRect({bw + 16, 0, nw, 40});
                    meta->setRect({bw + 16 + nw + 12, 0, std::max(0.f, b.rect().w - bw - nw - 28), 40});
                };
                addTable(list, tab_ == Albums);
            } else {
                addGrid(groups);
            }
        }
        body_->requestLayout();
        scroll_->contentChanged();
    }

    // Klasörler: the way up, the subfolders as cards, the folder's own songs.
    void buildFolders() {
        const auto tree = LocalLibrary::get().folderTree();
        shownFolder_.clear();
        shownName_.clear();
        if (!tree) {
            waitingForTree_ = true;
            body_->add<ui::Label>(tr(L"Klasörler hazırlanıyor…"), type::secondary, ui::Tone::Tertiary);
            return;
        }
        showsTree_ = true;
        // The open folder; one that is gone (renamed, emptied) gives way to its nearest parent that still holds songs.
        int node = -1;
        for (std::wstring p = folder_; !p.empty() && node < 0;) {
            node = tree->find(p);
            const size_t slash = p.find_last_of(L'\\');
            p = slash == std::wstring::npos || slash < 3 ? std::wstring() : p.substr(0, slash);
        }
        if (!folder_.empty()) {
            const std::wstring now = node >= 0 ? tree->nodes[static_cast<size_t>(node)].path : std::wstring();
            if (toLowerTr(now) != toLowerTr(folder_)) {
                folder_ = now;
                remember();
                if (ctx().router) ctx().router->retarget(route());
            }
        }
        if (node < 0 && tree->roots.size() == 1) node = tree->roots[0];   // one listed folder: it is the top level
        if (node < 0 && tree->roots.empty()) {
            body_->add<ui::Label>(tr(L"Eklenen klasörlerde şarkı yok."), type::secondary, ui::Tone::Tertiary);
            return;
        }
        const auto& nodes = tree->nodes;
        const auto& all = LocalLibrary::get().tracks();
        const local::FolderNode* open = node >= 0 ? &nodes[static_cast<size_t>(node)] : nullptr;
        shownFolder_ = open ? open->path : std::wstring();
        shownName_ = open ? open->name : std::wstring(tr(L"Tüm klasörler"));

        // Breadcrumb: "Tüm klasörler" (several listed folders) and every parent up to the listed folder.
        std::vector<FolderHead::Step> up;
        for (int p = open ? open->parent : -1; p >= 0; p = nodes[static_cast<size_t>(p)].parent)
            up.insert(up.begin(), {nodes[static_cast<size_t>(p)].name, nodes[static_cast<size_t>(p)].path});
        if (open && tree->roots.size() > 1) up.insert(up.begin(), {tr(L"Tüm klasörler"), L""});

        const std::vector<int>& kids = open ? open->children : tree->roots;
        int total = 0;
        int64_t ms = 0;
        if (open) {
            total = open->total;
            ms = open->totalMs;
        } else {
            for (int r : tree->roots) {
                total += nodes[static_cast<size_t>(r)].total;
                ms += nodes[static_cast<size_t>(r)].totalMs;
            }
        }
        std::wstring meta = kids.empty() ? std::wstring() : i18n::plural(L"{} klasör", static_cast<long long>(kids.size())) + L" · ";
        body_->add<FolderHead>(up, shownName_, meta + countLine(static_cast<size_t>(total), ms),
                               [](const std::wstring& path) { openFolder(path); });

        // Subfolders (the search box narrows them by name).
        const std::wstring needle = foldForSearch(filter_);
        std::vector<int> shown;
        for (int k : kids)
            if (needle.empty() || foldForSearch(nodes[static_cast<size_t>(k)].name).find(needle) != std::wstring::npos) shown.push_back(k);
        if (!shown.empty()) {
            auto* grid = body_->add<ui::Grid>(168.f, gfx::metrics::cardGap, [](float w) { return w + 10 + 18 + 4 + 16; });
            for (size_t i = 0; i < shown.size() && i < kMaxCards; ++i) {
                const local::FolderNode& f = nodes[static_cast<size_t>(shown[i])];
                const std::vector<Image> images = f.cover >= 0 && f.cover < static_cast<int>(all.size())
                                                      ? all[static_cast<size_t>(f.cover)].album.images
                                                      : std::vector<Image>{};
                auto* card = grid->add<MediaCard>(f.name, songCount(f.total), images, MediaCard::Shape::Square, Placeholder::Folder);
                card->onOpen = [path = f.path] { openFolder(path); };
                card->onPlay = [path = f.path, name = f.name] {
                    auto list = tracksUnder(path);
                    const int start = ctx().player->firstPlayable(list);
                    ctx().player->playContext(std::move(list), start, folderContext(path, name));
                };
                card->onContext = [path = f.path, name = f.name](gfx::Point wp) { showFolderMenu(path, name, wp); };
            }
            if (shown.size() > kMaxCards)
                body_->add<ui::Label>(i18n::plural(L"İlk {} sonuç gösteriliyor; aramayla daralt.", int64_t{kMaxCards}),
                                      type::secondary, ui::Tone::Tertiary);
        }

        // The folder's own songs.
        if (open && !open->tracks.empty()) {
            std::vector<Track> list;
            list.reserve(open->tracks.size());
            for (int i : open->tracks)
                if (i < static_cast<int>(all.size())) list.push_back(all[static_cast<size_t>(i)]);
            if (!kids.empty()) body_->add<SectionHeader>(tr(L"Bu klasördeki şarkılar"), songCount(static_cast<int64_t>(list.size())));
            addTable(list, false);
        } else if (shown.empty() && !needle.empty()) {
            body_->add<ui::Label>(tr(L"Aramanla eşleşen sonuç yok."), type::secondary, ui::Tone::Tertiary);
        }
    }

    // Opens a folder of the Klasörler view as a new page ("" = the top level), so Back returns here.
    static void openFolder(const std::wstring& path) {
        auto& s = Settings::get();
        s.localView = "folders";
        s.localFolder = toUtf8(path);
        s.markDirty();
        ctx().router->navigate({RouteKind::LocalFiles, path.empty() ? std::string("folders") : "folder:" + toUtf8(path)});
    }

    void addTable(const std::vector<Track>& list, bool albumOrder) {
        TrackTable::Options o;
        o.showAdded = false;
        o.showAlbum = !albumOrder;
        o.albumNumbering = albumOrder;
        table_ = body_->add<TrackTable>(o);
        table_->setTracks(list);
        if (!filter_.empty()) table_->setFilter(filter_);
        table_->onPlay = [this](int i) { ctx().player->playContext(table_->displayedTracks(), i, currentContext()); };
        noMatch_ = body_->add<ui::Label>(tr(L"Aramanla eşleşen şarkı yok."), type::secondary, ui::Tone::Tertiary);
        syncNoMatch();
    }

    // "No results" under a table the search emptied (its height is just the header then).
    void syncNoMatch() {
        if (!noMatch_ || !table_) return;
        const bool none = !filter_.empty() && table_->preferredHeight(0) <= table_->headerHeight() + 8.5f;
        if (none != noMatch_->visible()) {
            noMatch_->setVisible(none);
            body_->requestLayout();
        }
    }

    // Opens an album / artist of the grid. The grid's search stays with the grid (an album found by its artist or
    // year would otherwise open filtered down to nothing); inside the group the box searches its tracks.
    void openGroup(const std::string& key) {
        drillKey_ = key;
        gridFilter_ = filter_;
        filter_.clear();
        if (filterBox_) filterBox_->setText({}, false);
    }
    void closeGroup() {
        if (drillKey_.empty()) return;
        drillKey_.clear();
        filter_ = gridFilter_;
        gridFilter_.clear();
        if (filterBox_) filterBox_->setText(filter_, false);
    }

    // Each list plays under its own context, so the big Play button only pauses / resumes what this view started.
    player::PlayContext currentContext() const {
        if (tab_ == Folders) return folderContext(shownFolder_, shownName_);
        if (!drillKey_.empty()) return groupContext(drillKey_, drillTitle_);
        if (tab_ == Albums) return {"local:albums", tr(L"Yerel dosyalar")};
        if (tab_ == Artists) return {"local:artists", tr(L"Yerel dosyalar")};
        return {"local", tr(L"Yerel dosyalar")};
    }
    player::PlayContext groupContext(const std::string& key, const std::wstring& title) const {
        return {std::string(tab_ == Albums ? "local:album:" : "local:artist:") + key, title};
    }

    void addGrid(const std::vector<Group>& groups) {
        const std::wstring needle = foldForSearch(filter_);
        std::vector<int> shown;
        for (int i = 0; i < static_cast<int>(groups.size()); ++i)
            if (needle.empty() || foldForSearch(groups[i].title + L" " + groups[i].subtitle).find(needle) != std::wstring::npos)
                shown.push_back(i);
        if (shown.empty()) {
            body_->add<ui::Label>(needle.empty() ? (tab_ == Albums ? tr(L"Etiketlerinde albüm adı olan şarkı yok.")
                                                                   : tr(L"Etiketlerinde sanatçı adı olan şarkı yok."))
                                                 : tr(L"Aramanla eşleşen sonuç yok."),
                                  type::secondary, ui::Tone::Tertiary);
            return;
        }
        // Play / Karıştır on a grid: every track of the matching groups, in card order (each once).
        std::vector<char> taken(LocalLibrary::get().tracks().size(), 0);
        for (int gi : shown)
            for (int t : groups[gi].tracks)
                if (t < static_cast<int>(taken.size()) && !taken[t]) {
                    taken[t] = 1;
                    gridTracks_.push_back(t);
                }
        auto* grid = body_->add<ui::Grid>(168.f, gfx::metrics::cardGap, [](float w) { return w + 10 + 18 + 4 + 16; });
        const bool albums = tab_ == Albums;
        for (size_t k = 0; k < shown.size() && k < kMaxCards; ++k) {
            const int gi = shown[k];
            const Group& g = groups[gi];
            auto* card = grid->add<MediaCard>(g.title, g.subtitle, g.images, albums ? MediaCard::Shape::Square : MediaCard::Shape::Circle,
                                              albums ? Placeholder::Album : Placeholder::Artist);
            card->onOpen = [this, key = g.key] {
                openGroup(key);
                Dispatcher::post([this, ref = libLife_.ref()] {   // the card is destroyed by the rebuild
                    if (!ref.expired()) {
                        buildBody();
                        scroll_->scrollTo(0);
                    }
                });
            };
            const std::vector<int> idx = g.tracks;
            card->onPlay = [idx, context = groupContext(g.key, g.title)] {   // no row picked: the first track the player may pick
                auto tracks = tracksAt(idx);
                const int start = ctx().player->firstPlayable(tracks);
                ctx().player->playContext(std::move(tracks), start, context);
            };
            card->onContext = [idx](gfx::Point wp) { showTrackMenu(tracksAt(idx), wp); };
        }
        if (shown.size() > kMaxCards)
            body_->add<ui::Label>(i18n::plural(L"İlk {} sonuç gösteriliyor; aramayla daralt.", int64_t{kMaxCards}),
                                  type::secondary, ui::Tone::Tertiary);
    }

    static std::vector<Track> tracksAt(const std::vector<int>& idx) {
        const auto& tracks = LocalLibrary::get().tracks();
        std::vector<Track> out;
        for (int i : idx)
            if (i < static_cast<int>(tracks.size())) out.push_back(tracks[i]);
        return out;
    }

    // Albums: album tag + album artist (else the first artist), in library order (artist, album).
    static std::vector<Group> albumGroups() {
        const auto& lib = LocalLibrary::get();
        const auto& entries = lib.entries();
        const auto& tracks = lib.tracks();
        std::vector<Group> out;
        std::vector<std::pair<int, std::wstring>> yearArtist;   // per group: year (any track's), artist
        std::unordered_map<std::wstring, size_t> index;
        for (int i = 0; i < static_cast<int>(entries.size()) && i < static_cast<int>(tracks.size()); ++i) {
            const auto& e = entries[i];
            if (e.album.empty()) continue;
            const std::string& artist = !e.albumArtist.empty() ? e.albumArtist : e.artists.empty() ? std::string{} : e.artists[0];
            const std::wstring key = toLowerTr(toWide(e.album)) + L"\x1f" + toLowerTr(toWide(artist));
            auto [it, fresh] = index.emplace(key, out.size());
            if (fresh) {
                out.push_back({toUtf8(key), toWide(e.album), {}, {}, {}});
                yearArtist.emplace_back(0, toWide(artist));
            }
            Group& g = out[it->second];
            if (g.images.empty()) g.images = tracks[i].album.images;
            if (yearArtist[it->second].first == 0) yearArtist[it->second].first = e.year;
            g.tracks.push_back(i);
        }
        for (size_t k = 0; k < out.size(); ++k) {   // "2023 · Mavi Kuzey"
            const auto& [year, artist] = yearArtist[k];
            out[k].subtitle = year > 0 ? std::to_wstring(year) : std::wstring{};
            if (!artist.empty()) out[k].subtitle += (out[k].subtitle.empty() ? L"" : L" · ") + artist;
        }
        return out;
    }

    // Artists: every credited artist of a track, alphabetical.
    static std::vector<Group> artistGroups() {
        const auto& tracks = LocalLibrary::get().tracks();
        std::vector<Group> out;
        std::unordered_map<std::wstring, size_t> index;
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
            for (const auto& a : tracks[i].artists) {
                if (a.name.empty()) continue;
                auto [it, fresh] = index.emplace(toLowerTr(toWide(a.name)), out.size());
                if (fresh) out.push_back({toUtf8(it->first), toWide(a.name), {}, {}, {}});
                Group& g = out[it->second];
                if (g.images.empty()) g.images = tracks[i].album.images;
                g.tracks.push_back(i);
            }
        }
        for (auto& g : out) g.subtitle = songCount(static_cast<int64_t>(g.tracks.size()));
        std::sort(out.begin(), out.end(), [](const Group& a, const Group& b) {
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.title.c_str(), static_cast<int>(a.title.size()),
                                   b.title.c_str(), static_cast<int>(b.title.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
        return out;
    }

    void playAll(bool shuffle) {
        auto* p = ctx().player;
        const auto context = currentContext();
        if (!shuffle && p->context().uri == context.uri) {   // the play button toggles what this view started
            if (p->isPlaying()) {
                p->pause();
                return;
            }
            if (p->status() == player::Status::Paused) {
                p->play();
                return;
            }
        }
        // What the view shows; in Klasörler the open folder with its subfolders.
        auto list = tab_ == Folders ? (showsTree_ ? tracksUnder(shownFolder_) : std::vector<Track>{})
                    : table_        ? table_->displayedTracks()
                                    : tracksAt(gridTracks_);
        if (list.empty()) return;
        if (shuffle) p->setShuffle(true);
        // No row picked: -1 = a random / the first track the player may pick (blocked ones never start by themselves).
        const int start = shuffle ? -1 : p->firstPlayable(list);
        p->playContext(std::move(list), start, context);
    }

    Lifetime libLife_;
    Shape built_{};
    Tab tab_ = Songs;
    std::wstring folder_;           // Klasörler: the folder asked for ("" = the top level)
    std::wstring shownFolder_;      // ... the one shown ("" = every listed folder) and its name
    std::wstring shownName_;
    bool showsTree_ = false;        // the body shows the folder tree
    bool waitingForTree_ = false;   // ... or waits for it to be built
    Button* more_ = nullptr;
    std::string drillKey_;          // opened album / artist (Group::key), "" = the grid
    std::wstring drillTitle_;
    std::wstring filter_;           // the search box
    std::wstring gridFilter_;       // the grid's search while a group is open
    std::vector<int> gridTracks_;   // what Play plays on a grid
    ui::Label* status_ = nullptr;
    Button* rescan_ = nullptr;
    ui::PlayButton* play_ = nullptr;
    ui::TextBox* filterBox_ = nullptr;
    ui::Column* body_ = nullptr;
    TrackTable* table_ = nullptr;
    ui::Label* noMatch_ = nullptr;
};

// Ayarlar: "Kitaplığı tara" with a live status line (repaints on scan progress; the page is rebuilt when the
// library changes so the per-folder counts follow).
class ScanRow : public ui::Widget {
public:
    explicit ScanRow(std::function<void()> rebuild)
        : title_(tr(L"Klasörleri tara"), type::body), rebuild_(std::move(rebuild)) {
        hitTestVisible = false;
        button_ = add<Button>(ButtonKind::Secondary, tr(L"Yeniden tara"), "refresh");
        button_->onClick = [] { LocalLibrary::get().rescan(); };
        gen_ = LocalLibrary::get().generation();
        wasScanning_ = LocalLibrary::get().scanning();
        LocalLibrary::get().subscribe(life_.ref(), [this] {
            const auto& lib = LocalLibrary::get();
            // The folder rows follow: new counts, or a scan that ended ("Taranıyor…" -> "N şarkı" / unreachable).
            const bool ended = wasScanning_ && !lib.scanning();
            wasScanning_ = lib.scanning();
            if ((lib.generation() != gen_ || ended) && rebuild_) {
                gen_ = lib.generation();
                rebuild_();
            }
            sync();
        });
        sync();
    }
    float preferredHeight(float) override { return 64; }
    void layout() override { button_->setRect({rect().w - 140, rect().h * 0.5f - 20, 140, 40}); }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float tw = r.w - 140 - 48;
        c.text(title_, {r.x, r.y + 12, tw, 20}, col.fgPrimary);
        c.text(desc_, type::secondary, {r.x, r.y + 34, tw, 18}, col.fgSecondary);
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        paintChildren(c);
    }

private:
    void sync() {
        const auto& lib = LocalLibrary::get();
        const std::wstring status = scanLine();
        desc_ = countLine(lib.tracks().size(), lib.totalMs()) + L" · " + status + (status.ends_with(L'…') ? L" " : L". ") +
                tr(L"Açılışta değişenler kendiliğinden bulunur; değişmeyen dosyalar yeniden okunmaz.");
        button_->setLabel(lib.scanning() ? tr(L"Taranıyor…") : tr(L"Yeniden tara"));
        button_->setEnabled(!lib.scanning() && !Settings::get().localFolders.empty());
        invalidate();
    }

    gfx::Text title_;
    std::wstring desc_;
    Button* button_;
    std::function<void()> rebuild_;
    uint64_t gen_ = 0;
    bool wasScanning_ = false;
    Lifetime life_;
};

} // namespace

void initLocalLibrary() {
    // Offline playback: "local:" ids resolve to their file (asked by the Player before any YouTube lookup). A local
    // file never falls back to a YouTube search on its file name: one that is gone (renamed / moved, folder removed
    // or offline) resolves to a path that can't open, so the player reports an error and moves on.
    ctx().localFileResolvers.push_back([](const std::string& id) -> std::wstring {
        if (id.rfind("local:", 0) != 0) return {};
        std::wstring path = LocalLibrary::get().pathFor(id);
        return path.empty() ? L"<missing local file: " + toWide(id) + L">" : path;   // never shown; logged only
    });
    ctx().housekeepingHooks.push_back([] { LocalLibrary::get().tick(); });
    ctx().persistHooks.push_back([] { LocalLibrary::get().cancel(); });   // exit: don't keep a scan running
    LocalLibrary::get().start();
    tagedit::initDevHook();
}

void buildLocalFilesSection(ui::Column* c, const std::function<void()>& rebuild) {
    auto& lib = LocalLibrary::get();
    const auto folders = Settings::get().localFolders;   // copy: a click edits the list
    for (const auto& f : folders) {
        const std::wstring folder = toWide(f);
        const int n = lib.countUnder(folder);
        std::wstring desc;
        if (lib.folderUnavailable(folder))
            desc = i18n::plural(L"Klasöre ulaşılamıyor (sürücü takılı değil ya da ağ kapalı). "
                                L"{} şarkı önceki taramadan listeleniyor.", n);
        else if (lib.loaded() && lib.scannedFolder(folder)) desc = songCount(n);
        else desc = lib.scanning() ? tr(L"Taranıyor…") : tr(L"Henüz taranmadı");
        auto* row = c->add<SettingRow>(folder, desc);
        auto* rm = row->control<Button>(120.f, ButtonKind::Secondary, tr(L"Kaldır"), "trash");
        rm->setTooltip(tr(L"Listeden kaldır (dosyalara dokunulmaz)"));
        rm->onClick = [folder, rebuild] {
            LocalLibrary::get().removeFolder(folder);
            toast(tr(L"Klasör kaldırıldı; dosyaların yerinde duruyor"));
            rebuild();
        };
    }
    auto* add = c->add<SettingRow>(tr(L"Müzik klasörü ekle"),
                                   tr(L"Alt klasörler dahil taranır: MP3, M4A, AAC, FLAC, WAV, WMA. Şarkılar Yerel "
                                      L"dosyalar sayfasında etiketleri ve kapaklarıyla listelenir. Dosyaların yalnızca "
                                      L"sen etiketlerini düzenlediğinde değiştirilir."));
    auto* addBtn = add->control<Button>(140.f, ButtonKind::Secondary, tr(L"Klasör ekle"), "plus");
    addBtn->onClick = [rebuild] { addFolderPosted(rebuild); };
    if (!folders.empty()) c->add<ScanRow>(rebuild);
}

std::unique_ptr<Page> makeLocalFilesPage(const std::string& id) { return std::make_unique<LocalFilesPage>(id); }

} // namespace st::app
