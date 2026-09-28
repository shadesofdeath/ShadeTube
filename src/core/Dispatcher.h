#pragma once
// Marshals work onto the UI thread. The UI thread owns a message-only window; post() pushes a closure
// and wakes the message loop. Everything that touches widgets/Direct2D MUST run on the UI thread.
#include <functional>
#include <mutex>
#include <vector>

#include <windows.h>

namespace st {

class Dispatcher {
public:
    // Creates the message-only window on the calling thread (which becomes "the UI thread").
    static void init();
    static void shutdown();

    static void post(std::function<void()> fn);
    static bool isUiThread();

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static void drain();
};

} // namespace st
