#include "app/LoginWindow.h"

#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <windows.h>
#include <wrl.h>
// <wrl.h> must precede <WebView2.h>
#include <WebView2.h>

#include <string>

namespace st::app {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kClassName[] = L"ShadeTube.LoginWindow";
// Spotify sets sp_dc on .spotify.com after sign-in; open.spotify.com can read it. We land the WebView there.
constexpr wchar_t kStartUrl[] = L"https://accounts.spotify.com/en/login?continue=https%3A%2F%2Fopen.spotify.com%2F";
constexpr wchar_t kCookieUrl[] = L"https://open.spotify.com";

std::string takeUtf8(LPWSTR s) {
    if (!s) return {};
    std::string out = toUtf8(s);
    CoTaskMemFree(s);
    return out;
}

// One live login session. Self-owning: frees itself (deferred) once finished.
class Impl {
public:
    static void open(HWND parent, std::function<void(std::string)> onDone) {
        auto* self = new Impl(parent, std::move(onDone));
        if (!self->createWindow()) {
            self->finish({}); // finish() defers deletion
        }
    }

private:
    Impl(HWND parent, std::function<void(std::string)> onDone) : parent_(parent), onDone_(std::move(onDone)) {}

    bool createWindow() {
        static const ATOM atom = [] {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = &Impl::wndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = kClassName;
            wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
            return RegisterClassExW(&wc);
        }();
        if (!atom) return false;

        // Center a 520x760 client area over the parent (or the work area).
        RECT pr{};
        if (parent_ && GetWindowRect(parent_, &pr)) {
        } else {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &pr, 0);
        }
        RECT wr{0, 0, 520, 760};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        const int x = pr.left + ((pr.right - pr.left) - w) / 2;
        const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

        hwnd_ = CreateWindowExW(0, kClassName, tr(L"Spotify ile bağlan"), WS_OVERLAPPEDWINDOW, x, y, w, h, parent_, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;
        if (parent_) EnableWindow(parent_, FALSE); // modal
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        createWebView();
        return true;
    }

    void createWebView() {
        const std::wstring udf = (paths::appData() / L"webview2").wstring();
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, udf.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(r) || !env) {
                        ST_LOG_ERROR("spotify", "WebView2 environment failed: 0x{:08x}", static_cast<unsigned>(r));
                        finish({});
                        return S_OK;
                    }
                    env->CreateCoreWebView2Controller(
                        hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                   [this](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT {
                                       return onController(r2, controller);
                                   })
                                   .Get());
                    return S_OK;
                })
                .Get());
        if (FAILED(hr)) {
            ST_LOG_ERROR("spotify",
                         "WebView2 not available (0x{:08x}). Install the Evergreen WebView2 Runtime.",
                         static_cast<unsigned>(hr));
            finish({});
        }
    }

    HRESULT onController(HRESULT r, ICoreWebView2Controller* controller) {
        if (FAILED(r) || !controller) {
            finish({});
            return S_OK;
        }
        controller_ = controller;
        resizeWebView();
        controller_->get_CoreWebView2(&webview_);
        if (!webview_) {
            finish({});
            return S_OK;
        }
        // Re-check for the cookie after every completed navigation (login redirects through several pages).
        EventRegistrationToken tok{};
        webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    pollCookie();
                    return S_OK;
                })
                .Get(),
            &tok);
        // "Continue with Apple / Google / Facebook" opens a popup; keep it inside this same window so those
        // sign-in methods work (otherwise the flow would stall waiting for a window we never open).
        EventRegistrationToken nwr{};
        webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    args->get_Uri(&uri);
                    args->put_Handled(TRUE);
                    if (uri && *uri) sender->Navigate(uri);
                    if (uri) CoTaskMemFree(uri);
                    return S_OK;
                })
                .Get(),
            &nwr);
        webview_->Navigate(kStartUrl);
        return S_OK;
    }

    void pollCookie() {
        ComPtr<ICoreWebView2_2> wv2;
        if (FAILED(webview_.As(&wv2)) || !wv2) return;
        ComPtr<ICoreWebView2CookieManager> mgr;
        if (FAILED(wv2->get_CookieManager(&mgr)) || !mgr) return;
        mgr->GetCookies(kCookieUrl,
                        Callback<ICoreWebView2GetCookiesCompletedHandler>(
                            [this](HRESULT r, ICoreWebView2CookieList* list) -> HRESULT {
                                if (FAILED(r) || !list) return S_OK;
                                UINT count = 0;
                                list->get_Count(&count);
                                for (UINT i = 0; i < count; ++i) {
                                    ComPtr<ICoreWebView2Cookie> ck;
                                    if (FAILED(list->GetValueAtIndex(i, &ck)) || !ck) continue;
                                    LPWSTR name = nullptr;
                                    ck->get_Name(&name);
                                    const std::string n = takeUtf8(name);
                                    if (n != "sp_dc") continue;
                                    LPWSTR value = nullptr;
                                    ck->get_Value(&value);
                                    const std::string v = takeUtf8(value);
                                    if (!v.empty()) finish(v);
                                    return S_OK;
                                }
                                return S_OK;
                            })
                            .Get());
    }

    void resizeWebView() {
        if (!controller_ || !hwnd_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        controller_->put_Bounds(rc);
    }

    void finish(std::string cookie) {
        if (finished_) return;
        finished_ = true;
        cookie_ = std::move(cookie);
        // Tear down OUTSIDE the WebView2 event handler we're likely inside. Closing the controller synchronously
        // from a cookie/navigation callback makes EmbeddedBrowserWebView.dll fault (0xC0000005): it keeps using
        // the controller after our handler returns. Posting defers teardown to a clean UI-thread turn.
        Dispatcher::post([this] { teardown(); });
    }

    void teardown() {
        if (parent_) {
            EnableWindow(parent_, TRUE);
            SetForegroundWindow(parent_);
        }
        if (controller_) controller_->Close();
        controller_.Reset();
        webview_.Reset();
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        auto cb = std::move(onDone_);
        std::string cookie = std::move(cookie_);
        if (cb) cb(std::move(cookie));
        delete this;
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            switch (msg) {
            case WM_SIZE:
                self->resizeWebView();
                return 0;
            case WM_CLOSE:
                self->finish({});
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND parent_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    std::function<void(std::string)> onDone_;
    std::string cookie_;
    bool finished_ = false;
};

} // namespace

void LoginWindow::open(void* parentHwnd, std::function<void(std::string)> onDone) {
    Impl::open(reinterpret_cast<HWND>(parentHwnd), std::move(onDone));
}

} // namespace st::app
