// smtc_test - exercises app/Smtc against the real Windows media session service.
//
//   smtc_test                 init + metadata + timeline on a hidden window, pumps ~2 s, then reads it all back
//                             the way the flyout does (GlobalSystemMediaTransportControlsSessionManager, from an
//                             MTA thread), presses play/pause, next, previous, stop and seek remotely and checks
//                             that onButton / onSeek arrive on the UI thread; clear() hides the session; finally a
//                             press still queued for the UI thread when ~Smtc runs must never reach onButton.
//   smtc_test --no-verify     local calls only: exit 0 when init() succeeded and nothing threw.
//   smtc_test --hotkey-probe  additionally injects real VK_MEDIA_PLAY_PAUSE key presses (always in pairs, so any
//                             player that receives them ends in its original state) with and without a
//                             RegisterHotKey on the same key, and reports who received them.
//
// Exit codes: 0 ok, 1 init failed, 2 something threw, 3 a verification check failed.
#include "app/Smtc.h"
#include "catalog/Models.h"
#include "core/Dispatcher.h"

#include <windows.h>
#include <unknwn.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

using st::app::Smtc;
namespace gsmtc = winrt::Windows::Media::Control;
namespace wm = winrt::Windows::Media;
namespace wss = winrt::Windows::Storage::Streams;
using namespace std::chrono_literals;

namespace {

// UTF-8 source (/utf-8): the Turkish letters check the UTF-8 -> UTF-16 hop.
const std::string kTitle = "ShadeTube SMTC testi · Şarkı";
const std::string kArtistA = "Pink Floyd";
const std::string kArtistB = "İkinci Sanatçı";
const std::string kAlbum = "The Dark Side of the Moon";
constexpr int64_t kDurationMs = 240'000;
constexpr int64_t kPositionMs = 30'000;
// A real release group; catalog::coverArt() yields the 250/500/1200 Cover Art Archive URLs the app uses.
constexpr const char* kReleaseGroup = "f5093c06-23e3-404f-aeaa-40f72885ee3a";

DWORD g_uiThread = 0;
std::atomic<int> g_failures{0};
std::atomic<int> g_hotkeys{0};

struct Received {
    std::atomic<int> play{0}, pause{0}, next{0}, previous{0}, stop{0};
    std::atomic<int64_t> seek{-1};
    std::atomic<bool> offUiThread{false};
};

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

const char* name(Smtc::Button b) {
    switch (b) {
    case Smtc::Button::Play: return "Play";
    case Smtc::Button::Pause: return "Pause";
    case Smtc::Button::Next: return "Next";
    case Smtc::Button::Previous: return "Previous";
    case Smtc::Button::Stop: return "Stop";
    }
    return "?";
}

// UI-thread message pump; returns early once `until` holds.
void pumpFor(std::chrono::milliseconds duration, const std::function<bool()>& until = {}) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (until && until()) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// Worker-side wait (MTA thread; the UI thread keeps pumping meanwhile).
bool waitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 3s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return cond();
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY) {
        ++g_hotkeys;
        std::printf("  WM_HOTKEY id=%d\n", static_cast<int>(wp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND createHiddenWindow() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ShadeTube.SmtcTest";
    RegisterClassExW(&wc);
    // A real (never shown) top-level window: SMTC is per top-level window, message-only windows don't qualify.
    return CreateWindowExW(0, wc.lpszClassName, L"ShadeTube SMTC test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
}

gsmtc::GlobalSystemMediaTransportControlsSession findSession(
    const gsmtc::GlobalSystemMediaTransportControlsSessionManager& mgr, std::chrono::milliseconds timeout) {
    const winrt::hstring title = winrt::to_hstring(kTitle);
    gsmtc::GlobalSystemMediaTransportControlsSession found{nullptr};
    waitFor(
        [&] {
            for (const auto& s : mgr.GetSessions()) {
                const auto p = s.TryGetMediaPropertiesAsync().get();
                if (p && p.Title() == title) {
                    found = s;
                    return true;
                }
            }
            return false;
        },
        timeout);
    return found;
}

// Joins the MTA for the scope of a worker function. Declared before the function's try block, so every WinRT
// object created inside it (including on an early return) is released before the apartment is left.
struct MtaScope {
    MtaScope() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~MtaScope() { winrt::uninit_apartment(); }
    MtaScope(const MtaScope&) = delete;
    MtaScope& operator=(const MtaScope&) = delete;
};

// Runs on its own MTA thread (blocking .get() is not allowed on the STA UI thread).
void verifySession(Received& rx, std::atomic<bool>& playingSeen) {
    const MtaScope mta;
    try {
        const auto mgr = gsmtc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        const auto session = findSession(mgr, 8s);
        check(session != nullptr, "session is visible system-wide (what the flyout / lock screen read)");
        if (!session) return;
        std::printf("  source app id: %s\n", winrt::to_string(session.SourceAppUserModelId()).c_str());

        const auto props = session.TryGetMediaPropertiesAsync().get();
        check(props.Artist() == winrt::to_hstring(kArtistA + ", " + kArtistB), "artist line (UTF-8 -> UTF-16)");
        check(props.AlbumTitle() == winrt::to_hstring(kAlbum), "album title");
        check(props.AlbumArtist() == winrt::to_hstring(kArtistA), "album artist");
        check(props.TrackNumber() == 3, "track number");
        check(props.PlaybackType() && props.PlaybackType().Value() == wm::MediaPlaybackType::Music, "type = Music");

        const auto info = session.GetPlaybackInfo();
        check(info.PlaybackStatus() == gsmtc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing,
              "status = Playing");
        const auto controls = info.Controls();
        std::printf("  controls: play %d pause %d toggle %d stop %d next %d previous %d seek %d\n", controls.IsPlayEnabled(),
                    controls.IsPauseEnabled(), controls.IsPlayPauseToggleEnabled(), controls.IsStopEnabled(),
                    controls.IsNextEnabled(), controls.IsPreviousEnabled(), controls.IsPlaybackPositionEnabled());
        check(controls.IsPauseEnabled() && controls.IsPlayPauseToggleEnabled() && controls.IsStopEnabled(),
              "pause / toggle / stop enabled");
        check(controls.IsNextEnabled() && controls.IsPreviousEnabled(), "next/previous enabled");

        const auto tl = session.GetTimelineProperties();
        const auto endMs = std::chrono::duration_cast<std::chrono::milliseconds>(tl.EndTime()).count();
        const auto posMs = std::chrono::duration_cast<std::chrono::milliseconds>(tl.Position()).count();
        const auto maxSeekMs = std::chrono::duration_cast<std::chrono::milliseconds>(tl.MaxSeekTime()).count();
        std::printf("  timeline: position %lld ms / end %lld ms (max seek %lld ms)\n", static_cast<long long>(posMs),
                    static_cast<long long>(endMs), static_cast<long long>(maxSeekMs));
        check(endMs == kDurationMs && maxSeekMs == kDurationMs, "timeline end / max seek = duration");
        check(posMs >= kPositionMs && posMs < kPositionMs + 1000, "timeline position");

        // The thumbnail is a lazy http reference: opening it downloads the cover (this is what the shell does).
        wss::IRandomAccessStreamReference thumb{nullptr};
        const auto t0 = std::chrono::steady_clock::now();
        waitFor(
            [&] {
                thumb = session.TryGetMediaPropertiesAsync().get().Thumbnail();
                return thumb != nullptr;
            },
            10s);
        std::printf("  thumbnail reference after %lld ms\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count()));
        check(thumb != nullptr, "thumbnail set");
        if (thumb) {
            try {
                const auto stream = thumb.OpenReadAsync().get();
                wss::Buffer buf(16);
                const auto got = stream.ReadAsync(buf, 16, wss::InputStreamOptions::None).get();
                const uint8_t* d = got.data();
                const bool jpeg = got.Length() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
                const bool png = got.Length() >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
                std::printf("  thumbnail: %llu bytes, %s, content type '%s'\n",
                            static_cast<unsigned long long>(stream.Size()), jpeg ? "JPEG" : png ? "PNG" : "unknown",
                            winrt::to_string(stream.ContentType()).c_str());
                check(stream.Size() > 1000 && (jpeg || png), "thumbnail downloads and is an image");
            } catch (const winrt::hresult_error& e) {
                check(false, "thumbnail open: " + winrt::to_string(e.message()));
            }
        }

        // Remote presses == what the flyout / lock screen / media keys do. The test's onButton mimics the app
        // (Pause -> setPlaying(false) ...), so the round trip must come back as a status change too.
        const bool toggled = session.TryTogglePlayPauseAsync().get();
        check(toggled && waitFor([&] { return rx.pause > 0 || rx.play > 0; }), "toggle play/pause -> onButton");
        check(rx.pause > 0, "toggle while Playing arrives as Pause");
        check(waitFor([&] {
                  return session.GetPlaybackInfo().PlaybackStatus() ==
                         gsmtc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused;
              }),
              "setPlaying(false) -> status = Paused");
        check(session.TryPlayAsync().get() && waitFor([&] { return rx.play > 0; }), "play -> onButton(Play)");
        playingSeen = waitFor([&] {
            return session.GetPlaybackInfo().PlaybackStatus() ==
                   gsmtc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        });
        check(playingSeen, "setPlaying(true) -> status = Playing");
        check(session.TrySkipNextAsync().get() && waitFor([&] { return rx.next > 0; }), "next -> onButton(Next)");
        check(session.TrySkipPreviousAsync().get() && waitFor([&] { return rx.previous > 0; }),
              "previous -> onButton(Previous)");
        check(session.TryStopAsync().get() && waitFor([&] { return rx.stop > 0; }), "stop -> onButton(Stop)");
        const auto target = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(90s).count();
        check(session.TryChangePlaybackPositionAsync(target).get() && waitFor([&] { return rx.seek >= 0; }),
              "seek bar -> onSeek");
        check(rx.seek == 90'000, "onSeek position = 90000 ms (got " + std::to_string(rx.seek.load()) + ")");
        check(!rx.offUiThread, "callbacks ran on the UI thread");
    } catch (const winrt::hresult_error& e) {
        check(false, "verification threw: " + winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        check(false, std::string("verification threw: ") + e.what());
    }
}

void verifyCleared() {
    const MtaScope mta;
    try {
        const auto mgr = gsmtc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        const bool gone = waitFor(
            [&] {
                const winrt::hstring title = winrt::to_hstring(kTitle);
                for (const auto& s : mgr.GetSessions()) {
                    const auto p = s.TryGetMediaPropertiesAsync().get();
                    if (p && p.Title() == title) return false;
                }
                return true;
            },
            5s);
        check(gone, "clear() removes the session metadata");
    } catch (const winrt::hresult_error& e) {
        check(false, "clear verification threw: " + winrt::to_string(e.message()));
    }
}

// Runs `fn` on a worker while the UI thread keeps pumping (Smtc callbacks are dispatched there).
void runWhilePumping(std::function<void()> fn) {
    std::atomic<bool> done{false};
    std::thread worker([&] {
        fn();
        done = true;
    });
    pumpFor(60s, [&] { return done.load(); });
    worker.join();
    pumpFor(200ms);
}

st::catalog::Track testTrack() {
    st::catalog::Track t;
    t.id = "test:smtc";
    t.name = kTitle;
    t.artists = {{"", kArtistA}, {"", kArtistB}};
    t.album.id = kReleaseGroup;
    t.album.name = kAlbum;
    t.album.images = st::catalog::coverArt(kReleaseGroup);
    t.durationMs = static_cast<int>(kDurationMs);
    t.trackNumber = 3;
    return t;
}

// Lifetime: a press that is already queued for the UI thread when ~Smtc runs must be dropped, never delivered
// to onButton of the destroyed object. The UI thread pumps everything except the dispatcher's wake-up message
// (WM_APP + 1) while a worker presses play/pause, then the Smtc is destroyed before the queue is drained.
void lateButtonCheck(HWND hwnd) {
    constexpr UINT kDispatchMsg = WM_APP + 1;   // core/Dispatcher.cpp
    std::printf("\n-- lifetime: press queued, then ~Smtc --\n");
    std::atomic<int> late{0};
    bool queued = false;
    {
        Smtc s;
        if (!s.init(hwnd)) {
            check(false, "lifetime: init");
            return;
        }
        s.onButton = [&late](Smtc::Button) { ++late; };
        s.setTrack(testTrack());
        s.setPlaying(true);
        pumpFor(1s);

        std::atomic<bool> done{false}, pressed{false};
        std::thread worker([&] {
            const MtaScope mta;
            try {
                const auto mgr = gsmtc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                if (const auto session = findSession(mgr, 8s)) pressed = session.TryTogglePlayPauseAsync().get();
            } catch (...) {
            }
            done = true;
        });
        const auto pumpExceptDispatch = [&](std::chrono::milliseconds limit, bool untilDone) {
            const auto end = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < end && !(untilDone && done)) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, kDispatchMsg - 1, PM_REMOVE) ||
                       PeekMessageW(&m, nullptr, kDispatchMsg + 1, 0xFFFF, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                std::this_thread::sleep_for(5ms);
            }
        };
        pumpExceptDispatch(30s, true);
        pumpExceptDispatch(500ms, false);   // let a thread-pool handler that is still running post its press
        worker.join();
        check(pressed, "lifetime: remote play/pause accepted");
        MSG m;
        queued = PeekMessageW(&m, nullptr, kDispatchMsg, kDispatchMsg, PM_NOREMOVE) != 0;
        std::printf("  press %s queued for the UI thread when ~Smtc ran\n", queued ? "was" : "was NOT");
    }   // ~Smtc with the press still in the dispatcher queue
    pumpFor(500ms);   // drains it
    check(late == 0, "lifetime: no onButton after ~Smtc (got " + std::to_string(late.load()) + ")");
}

void pressPlayPause() {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MEDIA_PLAY_PAUSE;
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void hotkeyProbe(HWND hwnd, Received& rx) {
    std::printf("\n-- media key probe (VK_MEDIA_PLAY_PAUSE, two presses per round) --\n");
    const bool registered = RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE) != 0;
    std::printf("  RegisterHotKey: %s (error %lu)\n", registered ? "ok" : "failed", registered ? 0ul : GetLastError());
    for (int round = 0; round < 2; ++round) {
        const int hk0 = g_hotkeys, bt0 = rx.play + rx.pause;
        pressPlayPause();
        pumpFor(1500ms);
        pressPlayPause();
        pumpFor(1500ms);
        std::printf("  %s hotkey: WM_HOTKEY x%d, SMTC Play/Pause x%d\n", round == 0 ? "WITH" : "WITHOUT",
                    g_hotkeys - hk0, rx.play + rx.pause - bt0);
        if (round == 0 && registered) UnregisterHotKey(hwnd, 1);
    }
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool verify = true, probe = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--no-verify") verify = false;
        else if (a == "--hotkey-probe") probe = true;
    }

    // Same environment as the app's UI thread: COM STA + the dispatcher window.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_uiThread = GetCurrentThreadId();
    st::Dispatcher::init();

    int code = 0;
    try {
        HWND hwnd = createHiddenWindow();
        check(hwnd != nullptr, "hidden top-level window");

        Received rx;
        {
            Smtc smtc;
            check(!smtc.ready(), "inert before init");
            smtc.setTrack({});   // no-ops before init, must not throw
            smtc.setTimeline(1, 2);

            const bool ok = smtc.init(hwnd);
            check(ok, "init(hwnd)");
            check(smtc.init(hwnd), "init twice is a no-op");
            if (!ok) {
                code = 1;
            } else {
                smtc.onButton = [&](Smtc::Button b) {
                    if (GetCurrentThreadId() != g_uiThread) rx.offUiThread = true;
                    std::printf("  onButton(%s)\n", name(b));
                    switch (b) {
                    case Smtc::Button::Play: ++rx.play; smtc.setPlaying(true); break;
                    case Smtc::Button::Pause: ++rx.pause; smtc.setPlaying(false); break;
                    case Smtc::Button::Next: ++rx.next; break;
                    case Smtc::Button::Previous: ++rx.previous; break;
                    case Smtc::Button::Stop: ++rx.stop; break;
                    }
                };
                smtc.onSeek = [&](int64_t ms) {
                    if (GetCurrentThreadId() != g_uiThread) rx.offUiThread = true;
                    std::printf("  onSeek(%lld)\n", static_cast<long long>(ms));
                    rx.seek = ms;
                    smtc.setTimeline(ms, kDurationMs);
                };

                const st::catalog::Track t = testTrack();

                // UI-thread cost of each call (they must not block the message loop noticeably).
                const auto clk = [] { return std::chrono::steady_clock::now(); };
                const auto msSince = [&](std::chrono::steady_clock::time_point a) {
                    return std::chrono::duration<double, std::milli>(clk() - a).count();
                };
                auto t0 = clk();
                smtc.setTrack(t);
                const double trackMs = msSince(t0);
                smtc.setTrack(t);   // same track again: ignored
                smtc.setNavigation(true, true);
                t0 = clk();
                smtc.setPlaying(true);
                const double playingMs = msSince(t0);
                t0 = clk();
                smtc.setTimeline(kPositionMs, kDurationMs);
                const double timelineMs = msSince(t0);
                smtc.setTimeline(kPositionMs + 100, kDurationMs);   // < 250 ms drift: dropped
                std::printf("  UI-thread cost: setTrack %.1f ms, setPlaying %.1f ms, setTimeline %.1f ms\n", trackMs,
                            playingMs, timelineMs);
                std::printf("  cover: %s\n", st::catalog::pickImage(t.album.images, 480)->url.c_str());

                pumpFor(2s);

                if (verify) {
                    std::atomic<bool> playing{false};
                    runWhilePumping([&] { verifySession(rx, playing); });
                    if (probe) hotkeyProbe(hwnd, rx);
                    smtc.clear();
                    runWhilePumping([] { verifyCleared(); });
                } else {
                    if (probe) hotkeyProbe(hwnd, rx);
                    smtc.clear();
                    pumpFor(500ms);
                }
                smtc.clear();       // twice: no-op
                smtc.setTrack(t);   // re-shows the session
                pumpFor(300ms);
            }
        }   // ~Smtc: revokes the handlers and disables the controls
        pumpFor(300ms);
        if (verify && code == 0) lateButtonCheck(hwnd);
        if (hwnd) DestroyWindow(hwnd);
    } catch (const winrt::hresult_error& e) {
        std::printf("[FAIL] threw: %s\n", winrt::to_string(e.message()).c_str());
        code = 2;
    } catch (const std::exception& e) {
        std::printf("[FAIL] threw: %s\n", e.what());
        code = 2;
    } catch (...) {
        std::printf("[FAIL] threw an unknown exception\n");
        code = 2;
    }

    st::Dispatcher::shutdown();
    CoUninitialize();
    if (code == 0 && g_failures > 0) code = 3;
    std::printf("\n%s (exit %d, %d failed check%s)\n", code == 0 ? "OK" : "FAILED", code, g_failures.load(),
                g_failures == 1 ? "" : "s");
    return code;
}
