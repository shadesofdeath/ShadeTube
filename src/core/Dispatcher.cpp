#include "core/Dispatcher.h"

#include "core/Log.h"

#include <atomic>

namespace st {

namespace {
constexpr UINT WM_ST_DISPATCH = WM_APP + 1;
HWND g_hwnd = nullptr;
DWORD g_uiThread = 0;
std::mutex g_mutex;
std::vector<std::function<void()>> g_queue;
std::atomic<bool> g_signaled{false};
} // namespace

void Dispatcher::init() {
    g_uiThread = GetCurrentThreadId();
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Dispatcher::wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ShadeTube.Dispatcher";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

void Dispatcher::shutdown() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    std::lock_guard lock(g_mutex);
    g_queue.clear();
}

bool Dispatcher::isUiThread() { return GetCurrentThreadId() == g_uiThread; }

void Dispatcher::post(std::function<void()> fn) {
    {
        std::lock_guard lock(g_mutex);
        g_queue.push_back(std::move(fn));
    }
    // Coalesce wake-ups: one pending message is enough to drain the whole queue.
    if (!g_signaled.exchange(true) && g_hwnd) PostMessageW(g_hwnd, WM_ST_DISPATCH, 0, 0);
}

void Dispatcher::drain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lock(g_mutex);
        g_signaled = false;
        batch.swap(g_queue);
    }
    for (auto& fn : batch) {
        try {
            fn();
        } catch (const std::exception& e) {
            ST_LOG_ERROR("ui", "dispatched callback threw: {}", e.what());
        }
    }
}

LRESULT CALLBACK Dispatcher::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ST_DISPATCH) {
        drain();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace st
