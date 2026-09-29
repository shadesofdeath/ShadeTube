#pragma once
// Process-wide graphics objects: factories (D2D, DirectWrite, WIC) and ONE D3D11/D2D device shared by
// every window (main window + mini player), so device-dependent resources (bitmaps, icon masks) can be
// shared. WindowTarget owns the per-HWND flip-model swap chain and device context.
//
// Device loss: when EndDraw/Present reports D2DERR_RECREATE_TARGET / DXGI_ERROR_DEVICE_REMOVED the
// device is recreated and `generation()` increments; caches compare generations and drop stale resources.
//
// Idle release: while no window is on screen the app calls release(), which drops the device and everything created
// on it, so the driver hands back its memory (command buffers, shader and pipeline caches, swap chains, textures:
// tens of MB). Every holder of a device-dependent object therefore registers a release hook (DeviceHook) that resets
// it: one object left behind would keep the whole device alive. The next frame's ensure() creates a new device.
#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace st::gfx {

using Microsoft::WRL::ComPtr;

class Device {
public:
    static Device& get();

    void init();           // factories + device (call once on the UI thread)
    void recreate();       // after device loss
    void release();        // idle: drop the device and every device-dependent object (release hooks)
    void ensure();         // (re)create the device after release(); no-op while it exists
    bool ready() const { return d3d_ != nullptr; }
    void trim();           // release driver/D2D caches (window minimized)

    // Runs on the UI thread right before the device goes (release() or device loss). Returns a token for
    // removeReleaseHook(); objects that live shorter than the process use DeviceHook instead.
    int addReleaseHook(std::function<void()> hook);
    void removeReleaseHook(int token);

    // This process's use of the adapter's memory segments (IDXGIAdapter3::QueryVideoMemoryInfo): local = dedicated
    // video memory on a discrete GPU (on an integrated one, the shared system memory it treats as local).
    struct GpuMemory {
        uint64_t local = 0, nonLocal = 0;
    };
    GpuMemory gpuMemory() const;
    void shutdown();

    ID2D1Factory6* d2dFactory() const { return d2dFactory_.Get(); }
    IDWriteFactory7* dwrite() const { return dwrite_.Get(); }
    IWICImagingFactory2* wic() const { return wic_.Get(); }   // free-threaded: usable on workers
    ID3D11Device* d3d() const { return d3d_.Get(); }
    IDXGIDevice1* dxgi() const { return dxgi_.Get(); }
    ID2D1Device5* d2dDevice() const { return d2dDevice_.Get(); }

    // A resource-creation context (not bound to a window) for building bitmaps outside of a frame.
    ID2D1DeviceContext5* resourceContext() const { return resourceDc_.Get(); }

    uint64_t generation() const { return generation_; }

private:
    void createDevice();
    void dropDevice();   // runs the release hooks, then releases the D3D / D2D devices

    ComPtr<ID2D1Factory6> d2dFactory_;
    ComPtr<IDWriteFactory7> dwrite_;
    ComPtr<IWICImagingFactory2> wic_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice1> dxgi_;
    ComPtr<ID2D1Device5> d2dDevice_;
    ComPtr<ID2D1DeviceContext5> resourceDc_;
    uint64_t generation_ = 0;
    std::vector<std::pair<int, std::function<void()>>> releaseHooks_;
    int nextHook_ = 0;
};

// A release hook for the lifetime of its owner (a view holding a baked bitmap, a window's swap chain).
class DeviceHook {
public:
    explicit DeviceHook(std::function<void()> onRelease) : token_(Device::get().addReleaseHook(std::move(onRelease))) {}
    ~DeviceHook() { Device::get().removeReleaseHook(token_); }
    DeviceHook(const DeviceHook&) = delete;
    DeviceHook& operator=(const DeviceHook&) = delete;

private:
    int token_;
};

class WindowTarget {
public:
    explicit WindowTarget(HWND hwnd);
    ~WindowTarget();

    // Physical pixel size + DPI. Resizes the swap chain buffers when changed; the swap chain itself is created by the
    // first begin(), so a window that is never shown (autostart into the tray) never costs one.
    void resize(UINT widthPx, UINT heightPx, float dpi);

    // Returns nullptr when the window has no area (minimized). Must be paired with end(). Recreates the device
    // after an idle release.
    ID2D1DeviceContext5* begin();
    // Presents (vsync). Returns false if the device was lost (caller should redraw next frame).
    bool end();

    // Blocks until the swap chain can accept a new frame (low-latency pacing). Handle for MsgWait.
    HANDLE frameLatencyWaitable() const { return waitable_; }

    bool hasSwapChain() const { return swapChain_ != nullptr; }
    float dpi() const { return dpi_; }
    float scale() const { return dpi_ / 96.f; }
    UINT widthPx() const { return width_; }
    UINT heightPx() const { return height_; }

private:
    void createSwapChain();
    void createTargetBitmap();
    void releaseDeviceResources();

    HWND hwnd_;
    ComPtr<IDXGISwapChain2> swapChain_;
    ComPtr<ID2D1DeviceContext5> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    HANDLE waitable_ = nullptr;
    UINT width_ = 0, height_ = 0;
    float dpi_ = 96.f;
    uint64_t generation_ = 0;
    DeviceHook hook_{[this] { releaseDeviceResources(); }};
};

} // namespace st::gfx
