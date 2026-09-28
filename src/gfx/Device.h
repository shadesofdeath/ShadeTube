#pragma once
// Process-wide graphics objects: factories (D2D, DirectWrite, WIC) and ONE D3D11/D2D device shared by
// every window (main window + mini player), so device-dependent resources (bitmaps, icon masks) can be
// shared. WindowTarget owns the per-HWND flip-model swap chain and device context.
//
// Device loss: when EndDraw/Present reports D2DERR_RECREATE_TARGET / DXGI_ERROR_DEVICE_REMOVED the
// device is recreated and `generation()` increments; caches compare generations and drop stale resources.
#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

namespace st::gfx {

using Microsoft::WRL::ComPtr;

class Device {
public:
    static Device& get();

    void init();           // factories + device (call once on the UI thread)
    void recreate();       // after device loss
    void trim();           // release driver/D2D caches (window minimized)
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

    ComPtr<ID2D1Factory6> d2dFactory_;
    ComPtr<IDWriteFactory7> dwrite_;
    ComPtr<IWICImagingFactory2> wic_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice1> dxgi_;
    ComPtr<ID2D1Device5> d2dDevice_;
    ComPtr<ID2D1DeviceContext5> resourceDc_;
    uint64_t generation_ = 0;
};

class WindowTarget {
public:
    explicit WindowTarget(HWND hwnd);
    ~WindowTarget();

    // Physical pixel size + DPI. Recreates the swap chain buffers when changed.
    void resize(UINT widthPx, UINT heightPx, float dpi);

    // Returns nullptr when the window has no area (minimized). Must be paired with end().
    ID2D1DeviceContext5* begin();
    // Presents (vsync). Returns false if the device was lost (caller should redraw next frame).
    bool end();

    // Blocks until the swap chain can accept a new frame (low-latency pacing). Handle for MsgWait.
    HANDLE frameLatencyWaitable() const { return waitable_; }

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
};

} // namespace st::gfx
