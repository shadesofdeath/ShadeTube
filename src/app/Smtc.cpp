#include "app/Smtc.h"

#include "core/Async.h"
#include "core/Dispatcher.h"
#include "core/Log.h"

// Classic COM must be visible before C++/WinRT so winrt::guid_of / com_ptr work with the interop interface
// (WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in <unknwn.h>).
#include <unknwn.h>
#include <SystemMediaTransportControlsInterop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <utility>

namespace st::app {

namespace wf = winrt::Windows::Foundation;
namespace wm = winrt::Windows::Media;
namespace wss = winrt::Windows::Storage::Streams;

namespace {

constexpr const char* kTag = "smtc";
constexpr int kMaxLoggedErrors = 20;   // a broken session must not flood the log once per second

wf::TimeSpan toTimeSpan(int64_t ms) {
    return std::chrono::duration_cast<wf::TimeSpan>(std::chrono::milliseconds(ms));
}

// The cover shown by the flyout / lock screen: ~500 px is plenty (Spotify 640, Cover Art Archive 500) and avoids
// the multi-megabyte 1200 px originals. Windows fetches it lazily, so only http(s) URLs are usable.
std::string coverUrl(const catalog::Track& t) {
    const auto* img = catalog::pickImage(t.album.images, 480);
    if (!img) return {};
    const std::string& u = img->url;
    if (u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0) return u;
    return {};
}

std::string describe(const winrt::hresult_error& e) {
    return std::format("0x{:08X} {}", static_cast<uint32_t>(e.code().value), winrt::to_string(e.message()));
}

} // namespace

struct Smtc::Impl {
    wm::SystemMediaTransportControls controls{nullptr};
    winrt::event_token buttonToken{};
    winrt::event_token seekToken{};
    Lifetime life;                 // guards the callbacks posted to the UI thread
    int errors = 0;

    // Last values pushed to Windows (dedup; -1 = unknown).
    bool enabled = false;
    std::string trackKey;
    int playing = -1;
    int canPrevious = -1;
    int canNext = -1;
    int64_t position = -1;
    int64_t duration = -1;

    // Runs one batch of WinRT calls; logs and swallows every failure. Returns false when it failed.
    template <class F>
    bool call(const char* what, F&& fn) {
        try {
            fn();
            return true;
        } catch (const winrt::hresult_error& e) {
            if (++errors <= kMaxLoggedErrors) ST_LOG_WARN(kTag, "{} failed: {}", what, describe(e));
        } catch (const std::exception& e) {
            if (++errors <= kMaxLoggedErrors) ST_LOG_WARN(kTag, "{} failed: {}", what, e.what());
        } catch (...) {
            if (++errors <= kMaxLoggedErrors) ST_LOG_WARN(kTag, "{} failed", what);
        }
        return false;
    }

    void resetCache() {
        trackKey.clear();
        playing = canPrevious = canNext = -1;
        position = duration = -1;
    }
};

Smtc::Smtc() : impl_(std::make_unique<Impl>()) {}

Smtc::~Smtc() {
    if (!impl_ || !impl_->controls) return;
    auto& c = impl_->controls;
    impl_->call("shutdown", [&] {
        c.ButtonPressed(impl_->buttonToken);
        c.PlaybackPositionChangeRequested(impl_->seekToken);
        c.DisplayUpdater().ClearAll();
        c.DisplayUpdater().Update();
        c.PlaybackStatus(wm::MediaPlaybackStatus::Closed);
        c.IsEnabled(false);
    });
    c = nullptr;
}

bool Smtc::ready() const { return impl_ && impl_->controls; }

bool Smtc::init(HWND hwnd) {
    if (ready()) return true;
    if (!hwnd || !IsWindow(hwnd)) {
        ST_LOG_WARN(kTag, "init: no window");
        return false;
    }
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    if (FAILED(CoGetApartmentType(&apartment, &qualifier))) {
        ST_LOG_WARN(kTag, "init: COM is not initialized on this thread");
        return false;
    }

    wm::SystemMediaTransportControls controls{nullptr};
    winrt::event_token buttonToken{};
    winrt::event_token seekToken{};
    // The controls are a per-window singleton: a half-finished init must not leave a handler behind.
    const auto revoke = [&]() noexcept {
        if (!controls) return;
        try {
            if (buttonToken.value) controls.ButtonPressed(buttonToken);
            if (seekToken.value) controls.PlaybackPositionChangeRequested(seekToken);
        } catch (...) {
        }
    };
    try {
        auto interop = winrt::get_activation_factory<wm::SystemMediaTransportControls,
                                                     ISystemMediaTransportControlsInterop>();
        winrt::check_hresult(interop->GetForWindow(hwnd, winrt::guid_of<wm::SystemMediaTransportControls>(),
                                                   winrt::put_abi(controls)));
        if (!controls) throw winrt::hresult_error(E_NOINTERFACE);

        // Hidden until the first track: an empty "ShadeTube" card in the flyout helps nobody.
        controls.IsEnabled(false);
        controls.IsPlayEnabled(true);
        controls.IsPauseEnabled(true);
        controls.IsStopEnabled(true);
        controls.IsNextEnabled(true);
        controls.IsPreviousEnabled(true);
        controls.IsFastForwardEnabled(false);
        controls.IsRewindEnabled(false);
        controls.PlaybackStatus(wm::MediaPlaybackStatus::Closed);

        // Both events fire on a Windows thread-pool thread: hop to the UI thread, and only reach `self` while
        // the Lifetime (destroyed with impl_ in ~Smtc, also on the UI thread) is alive.
        const Lifetime::Ref guard = impl_->life.ref();
        Smtc* self = this;
        buttonToken = controls.ButtonPressed(
            [guard, self](const wm::SystemMediaTransportControls&,
                          const wm::SystemMediaTransportControlsButtonPressedEventArgs& args) {
                try {
                    Button b;
                    switch (args.Button()) {
                    case wm::SystemMediaTransportControlsButton::Play: b = Button::Play; break;
                    case wm::SystemMediaTransportControlsButton::Pause: b = Button::Pause; break;
                    case wm::SystemMediaTransportControlsButton::Next: b = Button::Next; break;
                    case wm::SystemMediaTransportControlsButton::Previous: b = Button::Previous; break;
                    case wm::SystemMediaTransportControlsButton::Stop: b = Button::Stop; break;
                    default: return;   // rewind / fast-forward / channel / record: not offered
                    }
                    Dispatcher::post([guard, self, b] {
                        if (guard.expired() || !self->onButton) return;
                        try {
                            self->onButton(b);
                        } catch (...) {
                            ST_LOG_ERROR(kTag, "onButton threw");
                        }
                    });
                } catch (...) {
                    // Never let anything escape into the WinRT event source.
                }
            });
        seekToken = controls.PlaybackPositionChangeRequested(
            [guard, self](const wm::SystemMediaTransportControls&,
                          const wm::PlaybackPositionChangeRequestedEventArgs& args) {
                try {
                    const int64_t ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(args.RequestedPlaybackPosition()).count();
                    Dispatcher::post([guard, self, ms] {
                        if (guard.expired() || !self->onSeek) return;
                        try {
                            self->onSeek(std::max<int64_t>(0, ms));
                        } catch (...) {
                            ST_LOG_ERROR(kTag, "onSeek threw");
                        }
                    });
                } catch (...) {
                }
            });
    } catch (const winrt::hresult_error& e) {
        ST_LOG_WARN(kTag, "init failed: {}", describe(e));
        revoke();
        return false;
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "init failed: {}", e.what());
        revoke();
        return false;
    } catch (...) {
        ST_LOG_WARN(kTag, "init failed");
        revoke();
        return false;
    }

    impl_->buttonToken = buttonToken;
    impl_->seekToken = seekToken;
    impl_->controls = std::move(controls);
    impl_->enabled = false;
    impl_->resetCache();
    impl_->canPrevious = impl_->canNext = 1;
    ST_LOG_INFO(kTag, "system media transport controls ready");
    return true;
}

void Smtc::setTrack(const catalog::Track& t) {
    if (!ready()) return;
    const std::string cover = coverUrl(t);
    const std::string artists = t.artistLine();
    std::string key = t.id + '\x1f' + t.name + '\x1f' + artists + '\x1f' + t.album.name + '\x1f' + cover;
    if (impl_->enabled && key == impl_->trackKey) return;

    auto& c = impl_->controls;
    const bool ok = impl_->call("setTrack", [&] {
        if (!impl_->enabled) {
            c.IsEnabled(true);
            impl_->enabled = true;
        }
        auto du = c.DisplayUpdater();
        du.ClearAll();                             // also drops the previous thumbnail
        du.Type(wm::MediaPlaybackType::Music);     // must be set (again) before MusicProperties
        auto mp = du.MusicProperties();
        mp.Title(winrt::to_hstring(t.name));
        mp.Artist(winrt::to_hstring(artists));
        mp.AlbumTitle(winrt::to_hstring(t.album.name));
        if (!t.artists.empty() && !t.artists.front().name.empty())
            mp.AlbumArtist(winrt::to_hstring(t.artists.front().name));
        if (t.trackNumber > 0) mp.TrackNumber(static_cast<uint32_t>(t.trackNumber));
        if (!cover.empty()) {
            // A bad URL only costs the thumbnail, never the metadata.
            impl_->call("thumbnail", [&] {
                du.Thumbnail(wss::RandomAccessStreamReference::CreateFromUri(wf::Uri(winrt::to_hstring(cover))));
            });
        }
        du.Update();
    });
    impl_->trackKey = ok ? std::move(key) : std::string{};
    impl_->position = impl_->duration = -1;   // the next timeline update always goes through
}

void Smtc::setPlaying(bool playing) {
    if (!ready() || impl_->playing == static_cast<int>(playing)) return;
    if (impl_->call("setPlaying", [&] {
            impl_->controls.PlaybackStatus(playing ? wm::MediaPlaybackStatus::Playing : wm::MediaPlaybackStatus::Paused);
        }))
        impl_->playing = playing ? 1 : 0;
}

void Smtc::setTimeline(int64_t positionMs, int64_t durationMs) {
    if (!ready()) return;
    if (durationMs <= 0) durationMs = positionMs = 0;
    positionMs = std::clamp<int64_t>(positionMs, 0, durationMs);
    if (durationMs == impl_->duration && impl_->position >= 0 && std::abs(positionMs - impl_->position) < 250) return;
    if (impl_->call("setTimeline", [&] {
            wm::SystemMediaTransportControlsTimelineProperties tl;
            tl.StartTime(toTimeSpan(0));
            tl.MinSeekTime(toTimeSpan(0));
            tl.EndTime(toTimeSpan(durationMs));
            tl.MaxSeekTime(toTimeSpan(durationMs));
            tl.Position(toTimeSpan(positionMs));
            impl_->controls.UpdateTimelineProperties(tl);
        })) {
        impl_->position = positionMs;
        impl_->duration = durationMs;
    }
}

void Smtc::setNavigation(bool canPrevious, bool canNext) {
    if (!ready()) return;
    if (impl_->canPrevious == static_cast<int>(canPrevious) && impl_->canNext == static_cast<int>(canNext)) return;
    if (impl_->call("setNavigation", [&] {
            impl_->controls.IsPreviousEnabled(canPrevious);
            impl_->controls.IsNextEnabled(canNext);
        })) {
        impl_->canPrevious = canPrevious ? 1 : 0;
        impl_->canNext = canNext ? 1 : 0;
    }
}

void Smtc::clear() {
    if (!ready()) return;
    if (!impl_->enabled && impl_->trackKey.empty()) return;
    auto& c = impl_->controls;
    impl_->call("clear", [&] {
        auto du = c.DisplayUpdater();
        du.ClearAll();
        du.Update();
        c.PlaybackStatus(wm::MediaPlaybackStatus::Closed);
        c.IsEnabled(false);
    });
    const int prev = impl_->canPrevious, next = impl_->canNext;
    impl_->enabled = false;
    impl_->resetCache();
    impl_->canPrevious = prev;   // the button flags themselves were not touched
    impl_->canNext = next;
}

} // namespace st::app
