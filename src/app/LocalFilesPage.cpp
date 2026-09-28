// Yerel dosyalar: the user's own music folders (app/LocalLibrary) — the page, the Ayarlar › YEREL MÜZİK rows and
// the wiring (player resolver, startup scan, exit).
#include "app/AppContext.h"
#include "app/LocalLibrary.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SettingsWidgets.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
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
    if (ms > 0) s += L" · " + totalDuration(ms);
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

// Album / artist group of the library (indices into LocalLibrary::tracks()).
struct Group {
    std::string key;   // stable identity (survives rescans): lower-cased album + album artist, or artist name
    std::wstring title, subtitle;
    std::vector<Image> images;
    std::vector<int> tracks;
};

class LocalFilesPage : public ScrollPage {
public:
    LocalFilesPage() {
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
    enum Tab { Songs, Albums, Artists };

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
        if (shape() != built_) {
            pendingScroll_ = scroll_->scrollY();   // restored by contentReady()
            build();
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
                                                L"listelenir, internet olmadan çalar. Dosyaların yalnızca okunur."),
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
        filterBox_ = bar->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Yerel dosyalarda ara"));
        filterBox_->setText(filter_);
        filterBox_->onChange = [this](const std::wstring& s) {
            filter_ = s;
            if (table_) {
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
        bar->onLayout = [p = play_, shuffle, box = filterBox_](ui::Box& b) {
            const float w = b.rect().w;
            p->setRect({0, 0, 56, 56});
            shuffle->setRect({56 + 16, 8, shuffle->naturalWidth(), 40});
            const float bw = std::min(280.f, std::max(160.f, w - (56 + 16 + shuffle->naturalWidth() + 24)));
            box->setRect({w - bw, 6, bw, 44});
        };

        auto* tabs = c->add<Tabs>(std::vector<std::wstring>{tr(L"Şarkılar"), tr(L"Albümler"), tr(L"Sanatçılar")}, tab_);
        tabs->onSelect = [this](int i) {
            closeGroup();
            tab_ = static_cast<Tab>(i);
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
        const auto& tracks = LocalLibrary::get().tracks();
        if (tab_ == Songs) {
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
        auto list = table_ ? table_->displayedTracks() : tracksAt(gridTracks_);   // what the view shows
        if (list.empty()) return;
        if (shuffle) p->setShuffle(true);
        // No row picked: -1 = a random / the first track the player may pick (blocked ones never start by themselves).
        const int start = shuffle ? -1 : p->firstPlayable(list);
        p->playContext(std::move(list), start, context);
    }

    Lifetime libLife_;
    Shape built_{};
    Tab tab_ = Songs;
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
                                      L"okunur, hiçbir zaman değiştirilmez."));
    auto* addBtn = add->control<Button>(140.f, ButtonKind::Secondary, tr(L"Klasör ekle"), "plus");
    addBtn->onClick = [rebuild] { addFolderPosted(rebuild); };
    if (!folders.empty()) c->add<ScanRow>(rebuild);
}

std::unique_ptr<Page> makeLocalFilesPage() { return std::make_unique<LocalFilesPage>(); }

} // namespace st::app
