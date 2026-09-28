#include "gfx/Device.h"

#include "core/Log.h"

#include <dxgi1_4.h>

#include <stdexcept>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxguid.lib")

namespace st::gfx {

static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        ST_LOG_ERROR("gfx", "{} failed: 0x{:08X}", what, static_cast<unsigned>(hr));
        throw std::runtime_error(std::string(what) + " failed");
    }
}

Device& Device::get() {
    static Device instance;
    return instance;
}

void Device::init() {
    D2D1_FACTORY_OPTIONS opts{};
#ifndef NDEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_WARNING;
#endif
    // Multi-threaded factory: worker threads create geometry/WIC bitmaps safely.
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory6), &opts,
                                   reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    if (FAILED(hr)) {  // debug layer not installed
        opts.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory6), &opts,
                               reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    }
    check(hr, "D2D1CreateFactory");
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory7),
                              reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())),
          "DWriteCreateFactory");
    check(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)),
          "WIC factory");
    createDevice();
}

void Device::createDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    if (FAILED(hr)) {
        ST_LOG_WARN("gfx", "hardware device unavailable (0x{:08X}), falling back to WARP", static_cast<unsigned>(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    }
    check(hr, "D3D11CreateDevice");
    check(d3d_.As(&dxgi_), "IDXGIDevice1");
    dxgi_->SetMaximumFrameLatency(1);
    check(d2dFactory_->CreateDevice(dxgi_.Get(), d2dDevice_.ReleaseAndGetAddressOf()), "ID2D1Device");
    // D2D caches intermediate effect textures up to this budget. On integrated GPUs those textures live in
    // system memory and count against the process, so keep the cache small (effects we use are cheap to redo).
    d2dDevice_->SetMaximumTextureMemory(24ull * 1024 * 1024);
    ComPtr<ID2D1DeviceContext> dc;
    check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc), "resource context");
    check(dc.As(&resourceDc_), "ID2D1DeviceContext5");
    ++generation_;
    ST_LOG_INFO("gfx", "graphics device created (generation {})", generation_);
}

void Device::trim() {
    if (d2dDevice_) d2dDevice_->ClearResources(0);
    ComPtr<IDXGIDevice3> dxgi3;
    if (dxgi_ && SUCCEEDED(dxgi_.As(&dxgi3))) dxgi3->Trim();
}

void Device::recreate() {
    resourceDc_.Reset();
    d2dDevice_.Reset();
    dxgi_.Reset();
    d3d_.Reset();
    createDevice();
}

void Device::shutdown() {
    resourceDc_.Reset();
    d2dDevice_.Reset();
    dxgi_.Reset();
    d3d_.Reset();
    wic_.Reset();
    dwrite_.Reset();
    d2dFactory_.Reset();
}

// ---------------------------------------------------------------------------------------------------

WindowTarget::WindowTarget(HWND hwnd) : hwnd_(hwnd) {}

WindowTarget::~WindowTarget() { releaseDeviceResources(); }

void WindowTarget::releaseDeviceResources() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    dc_.Reset();
    swapChain_.Reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
}

void WindowTarget::createSwapChain() {
    auto& dev = Device::get();
    ComPtr<IDXGIAdapter> adapter;
    dev.dxgi()->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max(1u, width_);
    desc.Height = std::max(1u, height_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> sc;
    HRESULT hr = factory->CreateSwapChainForHwnd(dev.d3d(), hwnd_, &desc, nullptr, nullptr, &sc);
    check(hr, "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    check(sc.As(&swapChain_), "IDXGISwapChain2");
    swapChain_->SetMaximumFrameLatency(1);
    waitable_ = swapChain_->GetFrameLatencyWaitableObject();

    ComPtr<ID2D1DeviceContext> dc;
    check(dev.d2dDevice()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc), "window context");
    check(dc.As(&dc_), "ID2D1DeviceContext5");
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    generation_ = dev.generation();
}

void WindowTarget::createTargetBitmap() {
    ComPtr<IDXGISurface> surface;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)), "GetBuffer");
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                               dpi_, dpi_);
    check(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, target_.ReleaseAndGetAddressOf()), "target bitmap");
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi_, dpi_);
}

void WindowTarget::resize(UINT w, UINT h, float dpi) {
    if (w == width_ && h == height_ && dpi == dpi_ && swapChain_) return;
    width_ = w;
    height_ = h;
    dpi_ = dpi;
    if (w == 0 || h == 0) return;
    if (!swapChain_ || generation_ != Device::get().generation()) {
        releaseDeviceResources();
        createSwapChain();
    } else {
        dc_->SetTarget(nullptr);
        target_.Reset();
        HRESULT hr = swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                                               DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(hr)) {
            releaseDeviceResources();
            createSwapChain();
        }
    }
    createTargetBitmap();
}

ID2D1DeviceContext5* WindowTarget::begin() {
    if (width_ == 0 || height_ == 0) return nullptr;
    if (!swapChain_ || generation_ != Device::get().generation()) {
        releaseDeviceResources();
        createSwapChain();
        createTargetBitmap();
    }
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return dc_.Get();
}

bool WindowTarget::end() {
    HRESULT hr = dc_->EndDraw();
    if (SUCCEEDED(hr)) {
        DXGI_PRESENT_PARAMETERS params{};
        hr = swapChain_->Present1(1, 0, &params);
    }
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ST_LOG_WARN("gfx", "device lost (0x{:08X}), recreating", static_cast<unsigned>(hr));
        releaseDeviceResources();
        Device::get().recreate();
        return false;
    }
    return true;
}

} // namespace st::gfx
