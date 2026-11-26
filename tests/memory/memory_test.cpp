// Developer test for the memory housekeeping of docs/DEVELOPMENT.md 3.2, offline, on the real graphics device.
//   buckets   ImageCache decodes at the next bucket of 64/128/192/256/384/512/768/1024 (then 1280), never above the
//             picture's own size
//   expiry    ImageCache::expire() keeps a bitmap painted in the last frames, on the current or the previous page scope,
//             or within idleMs; it drops the rest and gives their bytes back
//   icons     Icons::drawColored above 96 px draws from the SVG document (it does paint) and keeps no bitmap per size;
//             up to 96 px one cached bitmap per size as before
//   trimPool  released bitmaps' memory comes back after Device::trimPool() (where the driver charges them to the
//             process, as integrated GPUs do; the numbers are printed)
//   report    core/MemoryStats::detailedReport() names the split, the heaps, the stacks, the heap's block sizes
//   stats     ListenStats::readFile() leaves no capacity slack in the index and the plays
//   memory_test
#include "app/ListenStats.h"
#include "core/Dispatcher.h"
#include "core/MemoryStats.h"
#include "core/ThreadPool.h"
#include "gfx/Device.h"
#include "gfx/Icons.h"
#include "gfx/ImageCache.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

using namespace st;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

std::filesystem::path g_dir;

double mb(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

// A w x h PNG of stripes (something WIC has to scale).
std::filesystem::path writePng(const wchar_t* name, UINT w, UINT h) {
    const auto path = g_dir / name;
    auto* wic = gfx::Device::get().wic();
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 7);
            p[1] = static_cast<uint8_t>(y * 3);
            p[2] = static_cast<uint8_t>((x + y) & 0xFF);
            p[3] = 255;
        }
    gfx::ComPtr<IWICStream> stream;
    gfx::ComPtr<IWICBitmapEncoder> enc;
    gfx::ComPtr<IWICBitmapFrameEncode> frame;
    wic->CreateStream(&stream);
    stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WritePixels(h, w * 4, static_cast<UINT>(px.size()), px.data());
    frame->Commit();
    enc->Commit();
    return path;
}

std::string fileUrl(const std::filesystem::path& p) {
    std::string s = "file:///";
    for (wchar_t c : p.wstring()) s += c == L'\\' ? '/' : static_cast<char>(c);   // the temp path is ASCII
    return s;
}

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Requests `url` at `px` every "frame" until the bitmap is ready (or 10 s).
ID2D1Bitmap1* load(const std::string& url, int px) {
    auto& cache = gfx::ImageCache::get();
    const ULONGLONG until = GetTickCount64() + 10000;
    while (GetTickCount64() < until) {
        cache.beginFrame();
        if (auto* b = cache.request(url, px)) return b;
        pump();
        Sleep(2);
    }
    return nullptr;
}

size_t bucketCount(int bucket) {
    const auto s = gfx::ImageCache::get().stats();
    size_t i = 0;
    while (i < std::size(gfx::ImageCache::kBuckets) && gfx::ImageCache::kBuckets[i] < bucket) ++i;
    return s.bucketCount[i];
}

void testBuckets(const std::string& url) {
    std::printf("\n== buckets\n");
    auto& cache = gfx::ImageCache::get();
    cache.clear();
    ID2D1Bitmap1* b = load(url, 300);
    CHECK(b != nullptr);
    if (b) CHECK(b->GetPixelSize().width == 384 && b->GetPixelSize().height == 384);
    CHECK(bucketCount(384) == 1);
    CHECK(cache.memoryBytes() == 384u * 384 * 4);
    b = load(url, 129);
    if (b) CHECK(b->GetPixelSize().width == 192);
    b = load(url, 700);
    if (b) CHECK(b->GetPixelSize().width == 600);   // the bucket is 768: never above the picture's own 600 px
    CHECK(bucketCount(768) == 1);
    b = load(url, 64);
    if (b) CHECK(b->GetPixelSize().width == 64);
    CHECK(cache.stats().ready == 4);
    cache.clear();
}

void testExpiry(const std::string& url) {
    std::printf("\n== expiry\n");
    auto& cache = gfx::ImageCache::get();
    cache.clear();
    CHECK(load(url, 300) != nullptr);
    const size_t bytes = cache.memoryBytes();
    CHECK(bytes > 0);
    CHECK(cache.expire(0) == 0);   // painted in this frame
    for (int i = 0; i < 4; ++i) cache.beginFrame();
    CHECK(cache.expire(0) == 0);   // not painted lately, but on the current page
    cache.beginScope();
    CHECK(cache.expire(0) == 0);   // the previous page (Back stays instant)
    cache.beginScope();
    CHECK(cache.expire(60'000) == 0);   // two pages back, but painted less than a minute ago
    CHECK(cache.expire(0) == bytes);    // two pages back and idle: gone
    CHECK(cache.memoryBytes() == 0 && cache.stats().ready == 0);

    // Painted again on a later page (a cover in the player bar across navigations): it belongs to that page now.
    CHECK(load(url, 300) != nullptr);
    cache.beginScope();
    cache.beginScope();
    cache.beginFrame();
    CHECK(cache.request(url, 300) != nullptr);
    CHECK(cache.expire(0) == 0);
    for (int i = 0; i < 3; ++i) cache.beginFrame();
    CHECK(cache.expire(0) == 0);
    cache.beginScope();
    cache.beginScope();
    CHECK(cache.expire(0) > 0);
    // Expired bitmaps load again when painted.
    CHECK(load(url, 300) != nullptr);
    cache.clear();
}

void testIcons() {
    std::printf("\n== icons\n");
    auto& dev = gfx::Device::get();
    gfx::ComPtr<ID2D1DeviceContext> dc0;
    gfx::ComPtr<ID2D1DeviceContext5> dc;
    dev.d2dDevice()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc0);
    dc0.As(&dc);
    const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    gfx::ComPtr<ID2D1Bitmap1> target, readback;
    dc->CreateBitmap(D2D1::SizeU(600, 600), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt), &target);
    dc->CreateBitmap(D2D1::SizeU(600, 600), nullptr, 0,
                     D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt), &readback);
    dc->SetTarget(target.Get());
    gfx::Icons::clear();
    const char* name = "placeholders/album-placeholder";
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    gfx::Icons::drawColored(dc.Get(), name, {0, 0, 64, 64}, 1.f);
    const size_t smallCount = gfx::Icons::count(), smallBytes = gfx::Icons::memoryBytes();
    CHECK(smallCount == 1 && smallBytes == 64u * 64 * 4);   // a small one: one cached bitmap
    for (int px = 400; px < 430; ++px) gfx::Icons::drawColored(dc.Get(), name, {100, 100, float(px), float(px)}, 1.f);
    CHECK(gfx::Icons::count() == smallCount + 1);   // + the parsed document, once
    CHECK(gfx::Icons::memoryBytes() == smallBytes);  // no bitmap for any of the 30 sizes
    CHECK(SUCCEEDED(dc->EndDraw()));
    // It painted: the placeholder's surface color (opaque) in the middle of the big one.
    const D2D1_POINT_2U origin{0, 0};
    const D2D1_RECT_U all{0, 0, 600, 600};
    readback->CopyFromBitmap(&origin, target.Get(), &all);
    D2D1_MAPPED_RECT m{};
    if (SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &m))) {
        const uint8_t* center = m.bits + 300 * m.pitch + 300 * 4;
        const uint8_t* corner = m.bits + 590 * m.pitch + 590 * 4;   // outside every drawn rect
        std::printf("   center alpha %d, corner alpha %d\n", center[3], corner[3]);
        CHECK(center[3] > 200);
        CHECK(corner[3] == 0);
        readback->Unmap();
    }
    gfx::Icons::clear();
}

void testTrimPool() {
    std::printf("\n== trimPool\n");
    auto& dev = gfx::Device::get();
    auto* dc = dev.resourceContext();
    const uint64_t before = mem::take().privateCommit;
    std::vector<uint8_t> px(512 * 512 * 4, 0x80);
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    // As in the app: the bitmaps are released, frames go by (the GPU finished with them), then trimPool().
    auto gpuIdle = [&] {
        gfx::ComPtr<ID3D11DeviceContext> ctx;
        dev.d3d()->GetImmediateContext(&ctx);
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        gfx::ComPtr<ID3D11Query> q;
        if (FAILED(dev.d3d()->CreateQuery(&qd, &q))) return;
        ctx->End(q.Get());
        ctx->Flush();
        BOOL done = FALSE;
        for (int i = 0; i < 2000 && ctx->GetData(q.Get(), &done, sizeof done, 0) != S_OK; ++i) Sleep(1);
    };
    std::vector<gfx::ComPtr<ID2D1Bitmap1>> bitmaps(24);
    for (auto& b : bitmaps) dc->CreateBitmap(D2D1::SizeU(512, 512), px.data(), 512 * 4, &props, &b);
    gpuIdle();
    const uint64_t with = mem::take().privateCommit;
    bitmaps.clear();
    gpuIdle();
    const uint64_t released = mem::take().privateCommit;
    dev.trimPool();
    const uint64_t trimmed = mem::take().privateCommit;
    std::printf("   commit %.1f MB -> %.1f MB with 24 MB of bitmaps -> %.1f MB released -> %.1f MB after trimPool\n",
                mb(before), mb(with), mb(released), mb(trimmed));
    CHECK(trimmed <= released);
    if (with - before > 12ull << 20) {   // this GPU charges bitmaps to the process
        CHECK(released + (1ull << 20) >= with);              // releasing them alone gives nothing back...
        CHECK(released - trimmed > (with - before) / 2);     // ...trimPool does
    }
}

void testReport() {
    std::printf("\n== report\n");
    const std::string r = mem::detailedReport();
    std::printf("%s", r.substr(0, r.find('\n', r.find("private split")) + 1).c_str());
    CHECK(r.find("private split: heaps") != std::string::npos);
    CHECK(r.find("(default process heap") != std::string::npos);
    CHECK(r.find("thread stacks (committed / reserved KB): [") != std::string::npos);
    CHECK(r.find("default heap blocks by size") != std::string::npos);
    CHECK(r.find("largest mapped views") != std::string::npos);
    CHECK(r.back() == '\n');
}

void testStats() {
    std::printf("\n== stats\n");
    const auto file = g_dir / L"listening.json";
    {
        app::ListenStats s(file);
        s.load();
        for (int i = 0; i < 3001; ++i) {   // 3001 songs, two plays each: every vector grew past its size
            catalog::Track t;
            t.id = "local:" + std::to_string(i);
            t.name = "Song " + std::to_string(i);
            t.artists = {{"", "Artist " + std::to_string(i % 97)}};
            t.album.name = "Album " + std::to_string(i % 211);
            s.addPlay(t, 1'700'000'000 + i * 600, 120'000);
            s.addPlay(t, 1'800'000'000 + i * 600, 90'000);
        }
        CHECK(s.save());
    }
    const auto snap = app::ListenStats::readFile(file);
    const auto& ix = snap.index;
    std::printf("   %zu tracks, %zu plays, %zu artists, %zu albums\n", ix.tracks.size(), snap.plays.size(), ix.artists.size(),
                ix.albums.size());
    CHECK(ix.tracks.size() == 3001 && snap.plays.size() == 6002);
    CHECK(ix.tracks.capacity() == ix.tracks.size());
    CHECK(ix.trackArtists.capacity() == ix.trackArtists.size());
    CHECK(ix.trackAlbum.capacity() == ix.trackAlbum.size());
    CHECK(ix.artists.capacity() == ix.artists.size());
    CHECK(ix.albums.capacity() == ix.albums.size());
    CHECK(snap.plays.capacity() == snap.plays.size());
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_dir = std::filesystem::temp_directory_path() / ("memory_test_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(g_dir);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (g_dir / L"profile").c_str());   // never the user's profile
    ThreadPool pool(4);
    ThreadPool::setShared(&pool);
    Dispatcher::init();
    gfx::Device::get().init();
    const std::string url = fileUrl(writePng(L"cover.png", 600, 600));

    testBuckets(url);
    testExpiry(url);
    testIcons();
    testTrimPool();
    testReport();
    testStats();

    gfx::ImageCache::get().clear();
    gfx::Icons::clear();
    Dispatcher::shutdown();
    ThreadPool::setShared(nullptr);
    gfx::Device::get().shutdown();
    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
