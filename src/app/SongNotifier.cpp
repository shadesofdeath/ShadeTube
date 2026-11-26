// Song-change notification, the app part (see app/SongNotification.h): the player hook, whether a ShadeTube window is
// in view, the cover PNG and the WinRT toast with its "Sonraki" / "Beğen" buttons.
#include "app/SongNotification.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/InternetRadio.h"
#include "app/Installer.h"
#include "app/SettingsWidgets.h"
#include "app/ShareCard.h"
#include "app/Together.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <optional>

#pragma comment(lib, "dwmapi.lib")

namespace st::app {

namespace songnotify {
namespace {

namespace wun = winrt::Windows::UI::Notifications;
namespace fs = std::filesystem;
constexpr const char* kTag = "songnotify";

struct State {
    std::string pendingId;            // the song whose debounce runs
    double pendingAt = 0;             // steadyMs() of its change
    UINT_PTR timer = 0;
    std::deque<double> changes;       // steadyMs() of the recent track changes (sync bursts)
    double mediaControlAt = -1e12;    // steadyMs() of the last command from Windows' media controls
    std::string lastNotified;         // the song of the last notification
    int imageSlot = 0;
    Lifetime life;                    // cover renders
    wun::ToastNotifier notifier{nullptr};
    wun::ToastNotification shown{nullptr};   // kept alive: its Activated handler is how the buttons reach us
    bool winrtUnavailable = false;    // no toasts for this app id: the tray balloon instead
};
State& state() {
    static State s;
    return s;
}

std::string keyOf(const catalog::Track& t) { return t.id.empty() ? "name:" + t.name : t.id; }

// ---- Is a ShadeTube window in view? ---------------------------------------------------------------------------------

RECT frameOf(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    return r;
}

// `ours` lies under the foreground window `fg` (which is not ours), at least 90 % of it.
bool coveredBy(HWND ours, HWND fg) {
    if (!fg || !IsWindowVisible(fg) || IsIconic(fg)) return false;
    // Below ours in the z-order (e.g. the mini player stays on top of a maximized browser): not covering it.
    int guard = 0;
    for (HWND h = GetWindow(ours, GW_HWNDNEXT); h && guard < 4096; h = GetWindow(h, GW_HWNDNEXT), ++guard)
        if (h == fg) return false;
    const RECT a = frameOf(ours), b = frameOf(fg);
    RECT i{};
    if (!IntersectRect(&i, &a, &b)) return false;
    const double area = static_cast<double>(a.right - a.left) * (a.bottom - a.top);
    const double covered = static_cast<double>(i.right - i.left) * (i.bottom - i.top);
    return area > 0 && covered >= 0.9 * area;
}

bool appInView() {
    HWND fg = GetForegroundWindow();
    if (fg) fg = GetAncestor(fg, GA_ROOT);
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == GetCurrentProcessId()) return true;   // one of ours has the focus
    struct Search {
        HWND fg;
        bool inView = false;
    } search{fg};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM lp) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lp);
            if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
            BOOL cloaked = FALSE;   // on another virtual desktop
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;
            const RECT r = frameOf(h);
            if (r.right - r.left < 48 || r.bottom - r.top < 32) return TRUE;   // not a window one looks at
            if (coveredBy(h, s.fg)) return TRUE;
            s.inView = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.inView;
}

// Windows' own "not now": a full-screen app or game, presentation mode, quiet hours, a locked screen.
bool windowsBusy() {
    QUERY_USER_NOTIFICATION_STATE st{};
    if (FAILED(SHQueryUserNotificationState(&st))) return false;
    return st != QUNS_ACCEPTS_NOTIFICATIONS;
}

// ---- The toast ------------------------------------------------------------------------------------------------------

void onActivated(const std::wstring& args, const catalog::Track& t) {
    auto* p = ctx().player;
    if (args == L"next") {
        const auto* cur = p ? p->current() : nullptr;
        if (cur && keyOf(*cur) == keyOf(t)) p->next();   // a stale notification never skips another song
        else ST_LOG_INFO(kTag, "\"next\" from the notification of a song that is no longer playing: ignored");
    } else if (args == L"like") {
        auto& lib = ctx().library;
        if (lib.canLike(t.id) && !lib.isLiked(t.id)) lib.setLiked(t, true);
    } else {
        commands::run("show-window");   // the body: back to ShadeTube
    }
}

void balloon(const catalog::Track& t) {
    if (!ctx().trayBalloon) return;
    std::wstring text = toWide(t.artistLine());
    if (!t.album.name.empty()) text += (text.empty() ? L"" : L" · ") + toWide(t.album.name);
    ctx().trayBalloon(toWide(t.name), text, false);
}

void present(const catalog::Track& t, const fs::path& image) {
    auto& s = state();
    // Still the song playing (the cover took a moment)?
    const auto* cur = ctx().player ? ctx().player->current() : nullptr;
    if (!cur || keyOf(*cur) != keyOf(t)) return;
    auto& lib = ctx().library;
    Content c;
    c.title = toWide(t.name);
    c.artist = toWide(t.artistLine());
    c.album = toWide(t.album.name);
    c.image = image;
    c.likeButton = lib.canLike(t.id) && !lib.isLiked(t.id);
    c.nextLabel = tr(L"Sonraki");
    c.likeLabel = tr(L"Beğen");
    if (s.winrtUnavailable) return balloon(t);
    // Gone from the notification center when the song would have ended (at least a minute, at most 15).
    const int64_t duration = ctx().player->durationMs() > 0 ? ctx().player->durationMs() : t.durationMs;
    const int64_t left = std::clamp<int64_t>((duration - ctx().player->positionMs()) / 1000, 60, 900);
    try {
        if (!s.notifier) s.notifier = wun::ToastNotificationManager::CreateToastNotifier(installer::appUserModelId());
        const auto setting = s.notifier.Setting();
        if (setting != wun::NotificationSetting::Enabled) {
            // Turned off for ShadeTube or by policy: respected. Not registered (DisabledByManifest): the balloon.
            ST_LOG_INFO(kTag, "Windows notifications are off for the app ({})", static_cast<int>(setting));
            if (setting == wun::NotificationSetting::DisabledByManifest) {
                s.winrtUnavailable = true;
                balloon(t);
            }
            return;
        }
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(toastXml(c));
        wun::ToastNotification n(doc);
        n.Tag(L"song");
        n.Group(L"now-playing");
        n.ExpirationTime(winrt::clock::now() + std::chrono::seconds(left));
        n.Activated([t](const wun::ToastNotification&, const winrt::Windows::Foundation::IInspectable& args) {
            std::wstring a;
            if (const auto ta = args.try_as<wun::ToastActivatedEventArgs>()) a = ta.Arguments().c_str();
            Dispatcher::post([a, t] { onActivated(a, t); });
        });
        n.Failed([](const wun::ToastNotification&, const wun::ToastFailedEventArgs& e) {
            ST_LOG_WARN(kTag, "toast failed: 0x{:08X}", static_cast<uint32_t>(e.ErrorCode().value));
        });
        s.notifier.Show(n);
        s.shown = n;
        ST_LOG_INFO(kTag, "notified{}{}", image.empty() ? "" : " with the cover", c.likeButton ? ", like button" : "");
    } catch (const winrt::hresult_error& e) {
        ST_LOG_WARN(kTag, "toast unavailable (0x{:08X}): the tray balloon instead", static_cast<uint32_t>(e.code().value));
        s.notifier = nullptr;
        s.winrtUnavailable = true;
        balloon(t);
    }
}

// The cover as a small PNG the shell can read (two files in turn: the one on screen is never rewritten under it).
void notify(const catalog::Track& t) {
    auto& s = state();
    s.lastNotified = keyOf(t);
    const auto* img = catalog::pickImage(t.album.images, 300);
    if (!img || s.winrtUnavailable) return present(t, {});
    const fs::path file = installer::shellDir() / (L"now-playing-" + std::to_wstring(s.imageSlot++ % 2) + L".png");
    async(
        Priority::High, s.life.ref(),
        [url = img->url, file] {
            const auto px = gfx::ImageCache::decodePixels(url, 256);
            share::Pixels out;
            out.w = static_cast<unsigned>(px.width);
            out.h = static_cast<unsigned>(px.height);
            out.bgra.resize(static_cast<size_t>(out.w) * out.h * 4);
            for (unsigned y = 0; y < out.h; ++y)
                std::memcpy(out.bgra.data() + static_cast<size_t>(y) * out.w * 4, px.bgra.data() + static_cast<size_t>(y) * px.stride,
                            static_cast<size_t>(out.w) * 4);
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            if (!share::writePng(out, file.wstring())) throw std::runtime_error("cover PNG not written");
            return file;
        },
        [t](Result<fs::path> r) {
            if (!r) ST_LOG_INFO(kTag, "no cover for the notification: {}", r.errorMessage());
            present(t, r ? *r : fs::path{});
        });
}

void CALLBACK onTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    auto& s = state();
    if (id != s.timer) return;
    s.timer = 0;
    auto* p = ctx().player;
    const auto* cur = p ? p->current() : nullptr;
    if (!cur || keyOf(*cur) != s.pendingId) return;   // moved on: that change has a timer of its own
    const double now = steadyMs();
    while (!s.changes.empty() && s.changes.front() < now - kStormWindowMs) s.changes.pop_front();
    Input in;
    in.enabled = Settings::get().songNotifications;
    in.song = !radio::isStationId(cur->id) && !catalog::isPodcastId(cur->id) && !p->isLive();
    const auto st = p->status();
    in.playing = st == player::Status::Playing || st == player::Status::Buffering || st == player::Status::Resolving;
    in.repeat = s.pendingId == s.lastNotified;
    in.inView = appInView();
    in.sinceMediaControlMs = s.pendingAt - s.mediaControlAt;
    in.guest = together::guest();
    in.recentChanges = static_cast<int>(s.changes.size());
    in.busy = windowsBusy();
    const Verdict v = decide(in);
    if (v == Verdict::Off) return;
    ST_LOG_INFO(kTag, "song changed: {}", name(v));
    if (v == Verdict::Show) notify(*cur);
}

void onTrackChanged(const catalog::Track& t) {
    auto& s = state();
    const double now = steadyMs();
    s.changes.push_back(now);
    while (!s.changes.empty() && s.changes.front() < now - kStormWindowMs) s.changes.pop_front();
    if (!Settings::get().songNotifications) return;
    s.pendingId = keyOf(t);
    s.pendingAt = now;
    if (s.timer) KillTimer(nullptr, s.timer);
    s.timer = SetTimer(nullptr, 0, kDebounceMs, onTimer);
}

} // namespace

void noteMediaControl() { state().mediaControlAt = steadyMs(); }

} // namespace songnotify

void initSongNotifications() {
    ctx().trackChangedHooks.push_back([](const catalog::Track& t) { songnotify::onTrackChanged(t); });
    // Exit (and logoff): the notification of the song goes from the notification center (its buttons would have no one
    // to talk to), and the WinRT objects go while COM is still up; a later song recreates them.
    ctx().persistHooks.push_back([] {
        auto& s = songnotify::state();
        if (s.shown) {
            try {
                winrt::Windows::UI::Notifications::ToastNotificationManager::History().RemoveGroup(L"now-playing",
                                                                                               installer::appUserModelId());
            } catch (const winrt::hresult_error&) {
            }
        }
        s.shown = nullptr;
        s.notifier = nullptr;
    });
}

void buildSongNotificationRow(ui::Column* c, const std::function<void()>&) {
    settingsToggle(c, tr(L"Şarkı değişince bildirim göster"),
                   tr(L"ShadeTube penceresi görünmüyorken yeni bir şarkı başlayınca kapağı, adı ve sanatçısıyla bir "
                      L"Windows bildirimi çıkar; Sonraki ve Beğen düğmeleriyle. Mini oynatıcı ekrandayken, medya "
                      L"tuşlarıyla geçişlerde (Windows şarkıyı kendisi gösterir) ve tam ekran uygulamalarda çıkmaz."),
                   Settings::get().songNotifications, [](bool v) { Settings::get().songNotifications = v; });
}

} // namespace st::app
