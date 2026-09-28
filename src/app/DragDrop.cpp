#include "app/DragDrop.h"

#include "app/DroppedFiles.h"
#include "app/LocalLibrary.h"
#include "app/Shell.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <ole2.h>
#include <shellapi.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

namespace {

// ---- In-app / Explorer drag state ------------------------------------------------------------------------------

class DragGhost;

struct DragState {
    bool active = false;
    bool explorer = false;           // an OLE drag from another app (Explorer): OLE drives it, not the mouse capture
    bool demo = false;               // SHADETUBE_DRAG_DEMO: no mouse button is held
    DragPayload payload;
    gfx::Point pos{};
    DropTarget* target = nullptr;    // under the pointer (Shell chrome: lives as long as the window)
    bool accepted = false;           // `target` takes the payload
    DragGhost* ghost = nullptr;
    double lastTick = 0;
};
DragState g_drag;
Lifetime g_life;   // continuations of dropped-file reads
int g_reads = 0;   // dropped-file reads in flight

bool primaryButtonDown() {
    return (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
}

void cancelDrag();
void updateDrag(gfx::Point p);

// Follows the pointer: count + first title for songs, what the drop does for files. Never hit-testable.
class DragGhost : public ui::Widget {
public:
    DragGhost() { hitTestVisible = false; }
    ui::Widget* hitTest(gfx::Point) override { return nullptr; }

    void setPayload(const DragPayload& p) {
        if (p.fromExplorer()) return;
        title_ = gfx::Text(p.tracks.empty() ? std::wstring() : toWide(p.tracks.front().name), type::body.withSize(13));
        countText_ = i18n::number(static_cast<int64_t>(p.tracks.size()));
        count_ = gfx::Text(countText_, type::monoBadge);
    }

    void paint(Canvas& c) override {
        if (!g_drag.active) return;
        const auto& col = colors();
        const auto& acc = accent();
        const gfx::Point p = g_drag.pos;
        float x = std::round(p.x + 16), y = std::round(p.y + 12);
        if (g_drag.explorer) {
            // Over the queue: "Sıraya ekle"; anywhere else the files play at once.
            const bool queue = g_drag.accepted;
            label_.setText(queue ? tr(L"Sıraya ekle") : tr(L"Şimdi çal"));
            const float w = std::ceil(label_.measure().w) + 16 + 8 + 14 + 14;
            const Rect r{x, y, w, 32};
            c.shadow(r, 2, 24, 8, col.shadowToast);
            c.fillRounded(r, 2, col.bgElevated);
            c.strokeRounded(r, 2, acc.base);
            c.icon(queue ? "queue" : "play", {r.x + 12, r.cy() - 7, 14, 14}, acc.base);
            c.text(label_, {r.x + 12 + 14 + 8, r.y, w, r.h}, col.fgPrimary, gfx::VAlign::Center);
        } else {
            const float bw = std::max(18.f, std::ceil(count_.measure().w) + 10);
            const float tw = std::min(260.f, std::ceil(title_.measure().w));
            const Rect r{x, y, 10 + bw + 10 + tw + 14, 36};
            c.shadow(r, 2, 24, 8, col.shadowToast);
            c.fillRounded(r, 2, col.bgElevated);
            c.strokeRounded(r, 2, g_drag.accepted ? acc.base : col.hairStrong);
            const Rect badge{r.x + 10, r.cy() - 9, bw, 18};
            c.fillPill(badge, acc.base);
            c.text(countText_, type::monoBadge, badge, acc.onAccent, gfx::TextAlign::Center, gfx::VAlign::Center);
            c.text(title_, {badge.right() + 10, r.y, tw + 1, r.h}, col.fgPrimary, gfx::VAlign::Center);
        }
        // Keep ticking while the drag lasts: targets auto-scroll and open folders while the pointer rests on them,
        // and a release the source never saw (capture lost) ends the drag.
        ui::frame::requestNext();
        const double now = ui::frame::realNow();
        if (now - g_drag.lastTick >= 50) {
            g_drag.lastTick = now;
            Dispatcher::post([] {
                if (!g_drag.active) return;
                if (!g_drag.explorer && !g_drag.demo && !primaryButtonDown()) cancelDrag();
                else updateDrag(g_drag.pos);
            });
        }
    }

private:
    gfx::Text title_{{}, type::body}, count_{{}, type::monoBadge}, label_{{}, type::caption.withWeight(600)};
    std::wstring countText_;
};

// The drop target under the pointer (window DIPs), if any. Nothing takes drops behind a modal dialog.
DropTarget* targetAt(gfx::Point p) {
    auto* w = ctx().window;
    if (!w || !w->root() || w->hasModal()) return nullptr;
    for (ui::Widget* h = w->root()->hitTest(p); h; h = h->parent())
        if (auto* t = dynamic_cast<DropTarget*>(h)) return t;
    return nullptr;
}

void updateDrag(gfx::Point p) {
    g_drag.pos = p;
    DropTarget* t = targetAt(p);
    if (g_drag.target && g_drag.target != t) g_drag.target->dragLeave();
    g_drag.target = t;
    g_drag.accepted = t && t->dragOver(g_drag.payload, p);
    if (auto* w = ctx().window) w->invalidate();
}

void startDrag(DragPayload payload, gfx::Point p, bool explorer) {
    cancelDrag();
    auto* w = ctx().window;
    if (!w) return;
    g_drag.active = true;
    g_drag.explorer = explorer;
    g_drag.payload = std::move(payload);
    auto ghost = std::make_unique<DragGhost>();
    ghost->setPayload(g_drag.payload);
    g_drag.ghost = static_cast<DragGhost*>(w->pushOverlay(std::move(ghost), false));
    updateDrag(p);
}

void endDrag() {
    if (g_drag.target) g_drag.target->dragLeave();
    if (g_drag.ghost && ctx().window) ctx().window->removeOverlay(g_drag.ghost);
    g_drag = DragState{};
    if (auto* w = ctx().window) w->invalidate();
}

void cancelDrag() {
    if (g_drag.active) endDrag();
}

// Files dropped from Explorer: read on a worker, then play (queueIndex < 0) or join the queue.
void playDropped(std::vector<std::wstring> files, int queueIndex) {
    const bool folders = std::any_of(files.begin(), files.end(), [](const std::wstring& f) {
        const DWORD a = GetFileAttributesW(local::openablePath(f).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    });
    ++g_reads;
    // A big folder takes a while to read: say so when it isn't done within a second.
    if (folders)
        SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            if (g_reads > 0) toast(tr(L"Bırakılan klasörler taranıyor…"));
        });
    async(
        Priority::High, g_life.ref(), [files] { return dropped::collect(files, dropped::Store::coverDir()); },
        [queueIndex](Result<dropped::Collected> r) {
            --g_reads;
            if (!r || r->entries.empty()) {
                toast(tr(L"Bırakılanlarda çalınabilecek bir ses dosyası yok"), true);
                return;
            }
            dropped::Store::get().remember(r->entries);
            std::vector<catalog::Track> tracks;
            tracks.reserve(r->entries.size());
            for (const auto& e : r->entries) tracks.push_back(local::toTrack(e));
            auto* p = ctx().player;
            if (!p) return;
            if (queueIndex >= 0 && p->current()) {
                const int n = p->insertAt(queueIndex, tracks);
                if (n > 0) toast(i18n::plural(L"{} şarkı sıraya eklendi", n));
            } else {
                p->playContext(std::move(tracks), 0, {"dropped", tr(L"Bırakılan dosyalar")});
            }
            if (r->capped)
                toast(i18n::plural(L"Yalnızca ilk {} dosya alındı", static_cast<long long>(dropped::kMaxFiles)));
            // One folder that the local library doesn't cover yet: offer to keep it there.
            if (r->folders.size() != 1 || !ctx().window) return;
            const std::wstring folder = r->folders.front();
            for (const auto& f : Settings::get().localFolders)
                if (local::isUnder(folder, toWide(f))) return;
            ui::Toasts::show(ctx().window, tr(L"Bu klasör Yerel dosyalar'a eklensin mi?"), ui::ToastKind::Info, tr(L"Ekle"),
                             [folder] {
                                 if (LocalLibrary::get().addFolder(folder)) toast(tr(L"Klasör eklendi, şarkılar taranıyor…"));
                                 else toast(tr(L"Bu klasör zaten taranıyor"));
                             });
        });
}

// ---- Explorer (OLE) drop target ------------------------------------------------------------------------------------

std::vector<std::wstring> filesOf(IDataObject* data) {
    std::vector<std::wstring> out;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&fmt, &medium))) return out;
    if (auto* drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len, L'\0');
            DragQueryFileW(drop, i, path.data(), len + 1);
            if (!path.empty()) out.push_back(std::move(path));
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return out;
}

class ExplorerDrop : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                         IDropTarget> {
public:
    explicit ExplorerDrop(HWND hwnd) : hwnd_(hwnd) {}

    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        valid_ = false;
        auto files = filesOf(data);
        if (usable() && std::any_of(files.begin(), files.end(), dropped::droppable)) {
            valid_ = true;
            DragPayload payload;
            payload.files = std::move(files);
            startDrag(std::move(payload), toDip(pt), true);
        }
        *effect = valid_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* effect) override {
        if (valid_ && g_drag.active && g_drag.explorer) updateDrag(toDip(pt));
        *effect = valid_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        if (g_drag.explorer) cancelDrag();
        valid_ = false;
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL pt, DWORD* effect) override {
        *effect = valid_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (valid_ && g_drag.active && g_drag.explorer) dragdrop::finish(toDip(pt));
        valid_ = false;
        return S_OK;
    }

private:
    gfx::Point toDip(POINTL pt) const {
        POINT p{pt.x, pt.y};
        ScreenToClient(hwnd_, &p);
        const float s = ctx().window ? ctx().window->scale() : 1.f;
        return {static_cast<float>(p.x) / s, static_cast<float>(p.y) / s};
    }
    // Not behind a dialog, and not on the Spotify connect screen.
    static bool usable() {
        auto* w = ctx().window;
        if (!w || w->hasModal() || !ctx().player) return false;
        const auto* shell = dynamic_cast<const Shell*>(w->root());
        return shell && !shell->connectMode();
    }

    HWND hwnd_;
    bool valid_ = false;
};

} // namespace

// ---- dragdrop ----------------------------------------------------------------------------------------------------

namespace dragdrop {

void begin(std::vector<catalog::Track> tracks, gfx::Point windowPos) {
    if (tracks.empty()) return;
    DragPayload payload;
    payload.tracks = std::move(tracks);
    startDrag(std::move(payload), windowPos, false);
}

void move(gfx::Point windowPos) {
    if (g_drag.active && !g_drag.explorer) updateDrag(windowPos);
}

void finish(gfx::Point windowPos) {
    if (!g_drag.active) return;
    updateDrag(windowPos);
    DropTarget* target = g_drag.accepted ? g_drag.target : nullptr;
    const DragPayload payload = std::move(g_drag.payload);
    const bool explorer = g_drag.explorer;
    endDrag();
    if (target) target->drop(payload, windowPos);
    else if (explorer) playDropped(payload.files, -1);   // files dropped anywhere else play at once
}

void cancel() { cancelDrag(); }

bool active() { return g_drag.active; }

std::vector<catalog::Track> tracksForRow(const Route& row, const std::vector<catalog::Track>& tracks) {
    dropped::DropRow kind;
    if (row.kind == RouteKind::Liked) {
        kind = dropped::DropRow::Liked;
    } else if (row.kind == RouteKind::Playlist && source::loggedIn()) {
        if (!isEditableSpotifyPlaylist(row.id)) return {};
        kind = dropped::DropRow::SpotifyPlaylist;
    } else if (row.kind == RouteKind::Playlist && ctx().library.playlist(row.id)) {
        kind = dropped::DropRow::LocalPlaylist;
    } else {
        return {};
    }
    return dropped::droppableOn(kind, tracks, [](const std::string& id) { return ctx().library.canLike(id); });
}

void dropOnRow(const Route& row, const std::vector<catalog::Track>& tracks) {
    const auto take = tracksForRow(row, tracks);
    if (take.empty()) return;
    if (row.kind == RouteKind::Liked) {
        const int n = ctx().library.likeAll(take);
        toast(n > 0 ? i18n::plural(L"{} şarkı Beğenilen Şarkılar'a eklendi", n) : std::wstring(tr(L"Zaten Beğenilen Şarkılar'da")));
        return;
    }
    // A Spotify playlist gets every dragged song so the toast can note the ones that aren't on Spotify.
    addToPlaylistWithToast(row.id, source::loggedIn() ? tracks : take);
}

void dropOnQueue(int orderIndex, const DragPayload& payload) {
    if (payload.fromExplorer()) {
        playDropped(payload.files, orderIndex);
        return;
    }
    auto* p = ctx().player;
    if (!p) return;
    const bool wasEmpty = p->current() == nullptr;
    const int n = p->insertAt(orderIndex, payload.tracks);
    if (n > 0 && !wasEmpty) toast(i18n::plural(L"{} şarkı sıraya eklendi", n));
}

} // namespace dragdrop

namespace {

// Dev check without a mouse (docs/DEVELOPMENT.md): SHADETUBE_DRAG_DEMO="x,y[,queue][,drop]" drags the first Liked
// Songs (or, with SHADETUBE_DRAG_DEMO_FILES="path|path", those files as if from Explorer) to window point x,y 2.5 s
// after startup and holds it there (for --screenshot), or drops it a second later with "drop"; "queue" opens the queue
// panel first.
void scheduleDemo() {
    wchar_t spec[64] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_DRAG_DEMO", spec, 64)) return;
    static gfx::Point at{};
    static bool dropIt = false;
    at.x = static_cast<float>(_wtof(spec));
    const wchar_t* comma = wcschr(spec, L',');
    at.y = comma ? static_cast<float>(_wtof(comma + 1)) : 0.f;
    dropIt = wcsstr(spec, L"drop") != nullptr;
    static bool openQueue = false;
    openQueue = wcsstr(spec, L"queue") != nullptr;
    SetTimer(nullptr, 0, 2500, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        DragPayload payload;
        wchar_t files[4096] = {};
        if (GetEnvironmentVariableW(L"SHADETUBE_DRAG_DEMO_FILES", files, 4096)) {
            std::wstring all = files;
            for (size_t start = 0; start <= all.size();) {
                const size_t bar = std::min(all.find(L'|', start), all.size());
                if (bar > start) payload.files.push_back(all.substr(start, bar - start));
                start = bar + 1;
            }
        } else {
            const auto& liked = ctx().library.liked();
            payload.tracks.assign(liked.begin(), liked.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(3, liked.size())));
        }
        if (payload.files.empty() && payload.tracks.empty()) {
            ST_LOG_WARN("dragdrop", "drag demo: nothing to drag");
            return;
        }
        ST_LOG_INFO("dragdrop", "drag demo: {} songs / {} files to {},{}", payload.tracks.size(), payload.files.size(), at.x, at.y);
        if (openQueue && ctx().toggleQueue) ctx().toggleQueue(true);
        const bool explorer = payload.fromExplorer();
        startDrag(std::move(payload), {at.x - 60, at.y + 40}, explorer);
        g_drag.demo = true;
        updateDrag(at);
        if (!dropIt) return;
        SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR id2, DWORD) {
            KillTimer(nullptr, id2);
            ST_LOG_INFO("dragdrop", "drag demo: drop");
            dragdrop::finish(at);
        });
    });
}

} // namespace

void initDragDrop() {
    scheduleDemo();
    dropped::Store::get().load();
    // A dropped file outside the local library still plays after a restart (restored queue, history, stats). Asked
    // before the local library's resolver, which answers every "local:" id (an unknown one with a path that fails).
    auto& resolvers = ctx().localFileResolvers;
    resolvers.insert(resolvers.begin(), [](const std::string& id) -> std::wstring {
        return id.rfind("local:", 0) == 0 ? dropped::Store::get().pathFor(id) : std::wstring();
    });

    auto* w = ctx().window;
    if (!w) return;
    const HWND hwnd = w->hwnd();
    // OLE drag and drop needs OleInitialize on this (STA) thread; main.cpp only initialized COM.
    if (FAILED(OleInitialize(nullptr))) {
        ST_LOG_WARN("dragdrop", "OleInitialize failed: dropping files from Explorer is off");
        return;
    }
    const auto target = Microsoft::WRL::Make<ExplorerDrop>(hwnd);
    if (!target || FAILED(RegisterDragDrop(hwnd, target.Get()))) {
        ST_LOG_WARN("dragdrop", "RegisterDragDrop failed: dropping files from Explorer is off");
        return;
    }
    // The registration holds the target; it must be revoked before the window goes away.
    auto previous = w->onMessage;
    w->onMessage = [previous, hwnd](UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_DESTROY) RevokeDragDrop(hwnd);
        if (previous) previous(msg, wp, lp);
    };
}

} // namespace st::app
