#include "gfx/ImageCache.h"

#include "core/Dispatcher.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace st::gfx {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kStaleFrames = 120;   // ~2 s at 60 fps (frames only advance while rendering)
// Downloads are bounded: a station logo from the radio directory is any URL someone submitted (an endless audio stream,
// a huge file). Real artwork (Spotify, Cover Art Archive front-1200, Wikimedia thumbnails) is far below both limits.
constexpr size_t kMaxDownloadBytes = 16 * 1024 * 1024;
constexpr auto kDownloadTimeout = std::chrono::seconds(45);
constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;   // decoded size limit (a small file can claim 30000 x 30000)

int bucketFor(int px) {
    for (int b : {64, 128, 256, 512, 1024})
        if (px <= b) return b;
    return 1280;
}

std::string hashName(const std::string& url) {
    uint64_t h = 1469598103934665603ULL;   // FNV-1a 64
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

struct Decoded {
    std::vector<uint8_t> pixels;
    UINT32 width = 0, height = 0, stride = 0;
    std::optional<Color> accent;
};

// "file:///C:/Music/a%20b.jpg" -> L"C:\\Music\\a b.jpg" (percent-decoded UTF-8); "" for any other URL.
std::wstring localFilePath(const std::string& url) {
    if (url.rfind("file:///", 0) != 0) return {};
    std::string p;
    for (size_t i = 8; i < url.size(); ++i) {
        if (url[i] == '%' && i + 2 < url.size() && isxdigit(static_cast<unsigned char>(url[i + 1])) &&
            isxdigit(static_cast<unsigned char>(url[i + 2]))) {
            p += static_cast<char>(std::stoi(url.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            p += url[i] == '/' ? '\\' : url[i];
        }
    }
    return toWide(p);
}

// What an image server may answer with. Only types that are certainly not an image are refused before the body is read
// (servers label images loosely: octet-stream, text/plain for .ico...); WIC decides about the rest.
bool imageContentType(std::string_view type) {
    std::string t(type.substr(0, type.find(';')));
    for (char& c : t)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return !t.starts_with("audio/") && !t.starts_with("video/") && t != "text/html" && t != "application/ogg" &&
           t.find("mpegurl") == std::string::npos && t != "application/json";
}

std::string loadBytes(const std::string& url) {
    // Local artwork (covers of the user's own music files, app/LocalLibrary): read in place, never copied into the
    // disk cache.
    if (const std::wstring local = localFilePath(url); !local.empty()) {
        std::ifstream f(fs::path(local), std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty()) throw std::runtime_error("local image unreadable");
        return data;
    }
    const auto file = paths::imageCacheDir() / hashName(url);
    {
        std::ifstream f(file, std::ios::binary);
        if (f) {
            std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!data.empty()) {
                std::error_code ec;
                fs::last_write_time(file, fs::file_time_type::clock::now(), ec);   // LRU for pruning
                return data;
            }
        }
    }
    // Never into the user's own network (a logo URL like http://192.168.1.1/...); names are not resolved here.
    if (http::isLocalUrl(url, false)) throw std::runtime_error("local network address");
    auto resp = http::getLimited(url, {kMaxDownloadBytes, kDownloadTimeout, imageContentType});
    if (!resp.isSuccessStatusCode() || resp.body.empty())
        throw std::runtime_error("image HTTP " + std::to_string(resp.statusCode));
    std::ofstream(file, std::ios::binary).write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
    return std::move(resp.body);
}

Decoded decode(const std::string& bytes, int targetWidth) {
    auto* wic = Device::get().wic();
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                            static_cast<DWORD>(bytes.size()))))
        throw std::runtime_error("WIC stream");
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        throw std::runtime_error("unsupported image format");
    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) throw std::runtime_error("empty image");
    if (static_cast<uint64_t>(w) * h > kMaxPixels) throw std::runtime_error("image too large");

    ComPtr<IWICBitmapSource> source = frame;
    if (static_cast<int>(w) > targetWidth) {   // never upscale
        const UINT tw = static_cast<UINT>(targetWidth);
        const UINT th = std::max(1u, static_cast<UINT>(std::lround(static_cast<double>(h) * tw / w)));
        ComPtr<IWICBitmapScaler> scaler;
        wic->CreateBitmapScaler(&scaler);
        scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeHighQualityCubic);
        source = scaler;
        w = tw;
        h = th;
    }
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        throw std::runtime_error("pixel format conversion");

    Decoded d;
    d.width = w;
    d.height = h;
    d.stride = w * 4;
    d.pixels.resize(static_cast<size_t>(d.stride) * h);
    if (FAILED(conv->CopyPixels(nullptr, d.stride, static_cast<UINT>(d.pixels.size()), d.pixels.data())))
        throw std::runtime_error("CopyPixels");
    d.accent = extractAccent(d.pixels.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(d.stride));
    return d;
}

} // namespace

// ---------------------------------------------------------------------------------------------------

struct ImageCache::Impl {
    enum class State { Pending, Ready, Failed };
    struct Entry {
        State state = State::Pending;
        ComPtr<ID2D1Bitmap1> bitmap;
        uint64_t generation = 0;
        size_t bytes = 0;
        std::shared_ptr<std::atomic<uint64_t>> stamp = std::make_shared<std::atomic<uint64_t>>(0);
        std::list<std::string>::iterator lruIt;
        bool inLru = false;
    };

    std::unordered_map<std::string, Entry> entries;
    std::list<std::string> lru;   // front = most recently used (Ready entries only)
    std::unordered_map<std::string, Color> accents;
    std::unordered_map<std::string, std::vector<std::function<void(std::optional<Color>)>>> accentWaiters;
    std::shared_ptr<std::atomic<uint64_t>> frame = std::make_shared<std::atomic<uint64_t>>(0);
    size_t budget = 96ull * 1024 * 1024;
    size_t used = 0;

    void touch(const std::string& key, Entry& e) {
        if (e.inLru) lru.erase(e.lruIt);
        lru.push_front(key);
        e.lruIt = lru.begin();
        e.inLru = true;
    }

    void evictTo(size_t target) {
        while (used > target && !lru.empty()) {
            const std::string key = lru.back();
            lru.pop_back();
            auto it = entries.find(key);
            if (it != entries.end()) {
                used -= it->second.bytes;
                entries.erase(it);
            }
        }
    }

    void finishAccent(const std::string& url, std::optional<Color> c) {
        if (c) accents[url] = *c;
        if (auto it = accentWaiters.find(url); it != accentWaiters.end()) {
            auto waiters = std::move(it->second);
            accentWaiters.erase(it);
            for (auto& w : waiters) w(c);
        }
    }

    void load(const std::string& key, const std::string& url, int bucket, Priority prio) {
        auto stamp = entries[key].stamp;
        auto frameNow = frame;
        ThreadPool::shared().post(
            [this, key, url, bucket, stamp, frameNow] {
                if (static_cast<int64_t>(frameNow->load() - stamp->load()) > static_cast<int64_t>(kStaleFrames)) {
                    Dispatcher::post([this, key] { entries.erase(key); });   // re-requested later if visible again
                    return;
                }
                std::shared_ptr<Decoded> decoded;
                try {
                    decoded = std::make_shared<Decoded>(decode(loadBytes(url), bucket));
                } catch (const std::exception& e) {
                    ST_LOG_DEBUG("images", "load failed {}: {}", url, e.what());
                }
                Dispatcher::post([this, key, url, decoded] {
                    auto it = entries.find(key);
                    if (it == entries.end()) return;
                    auto& e = it->second;
                    if (!decoded) {
                        e.state = State::Failed;
                        finishAccent(url, std::nullopt);
                        return;
                    }
                    const auto props = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    auto* dc = Device::get().resourceContext();
                    if (FAILED(dc->CreateBitmap(D2D1::SizeU(decoded->width, decoded->height), decoded->pixels.data(),
                                                decoded->stride, &props, &e.bitmap))) {
                        e.state = State::Failed;
                        return;
                    }
                    e.state = State::Ready;
                    e.generation = Device::get().generation();
                    e.bytes = decoded->pixels.size();
                    used += e.bytes;
                    touch(key, e);
                    finishAccent(url, decoded->accent);
                    evictTo(budget);
                    if (auto& cb = ImageCache::get().onLoaded) cb();
                });
            },
            prio);
    }
};

ImageCache& ImageCache::get() {
    static ImageCache instance;
    return instance;
}

ImageCache::Impl& ImageCache::impl() const {
    static Impl instance;
    return instance;
}

void ImageCache::beginFrame() { impl().frame->fetch_add(1); }

ID2D1Bitmap1* ImageCache::request(const std::string& url, int px, Priority priority) {
    if (url.empty()) return nullptr;
    auto& im = impl();
    const int bucket = bucketFor(px);
    std::string key = url;
    key += '#';
    key += std::to_string(bucket);

    auto it = im.entries.find(key);
    if (it == im.entries.end()) {
        auto& e = im.entries[key];
        e.stamp->store(im.frame->load());
        im.load(key, url, bucket, priority);
        return nullptr;
    }
    auto& e = it->second;
    e.stamp->store(im.frame->load());
    if (e.state != Impl::State::Ready) return nullptr;
    if (e.generation != Device::get().generation()) {   // device lost: reload
        im.used -= e.bytes;
        if (e.inLru) im.lru.erase(e.lruIt);
        im.entries.erase(it);
        return request(url, px, priority);
    }
    im.touch(key, e);
    return e.bitmap.Get();
}

bool ImageCache::isFailed(const std::string& url, int px) const {
    auto& im = impl();
    auto it = im.entries.find(url + '#' + std::to_string(bucketFor(px)));
    return it != im.entries.end() && it->second.state == Impl::State::Failed;
}

std::optional<Color> ImageCache::accentOf(const std::string& url) const {
    auto& im = impl();
    if (auto it = im.accents.find(url); it != im.accents.end()) return it->second;
    return std::nullopt;
}

void ImageCache::fetchAccent(const std::string& url, std::function<void(std::optional<Color>)> done) {
    auto& im = impl();
    if (auto c = accentOf(url)) {
        done(c);
        return;
    }
    im.accentWaiters[url].push_back(std::move(done));
    // A small bucket is enough for color extraction and shares the player-bar thumbnail.
    const std::string key = url + "#128";
    auto it = im.entries.find(key);
    if (it == im.entries.end()) {
        // Stamped in the future: an accent fetch is never considered stale.
        im.entries[key].stamp->store(im.frame->load() + 1'000'000);
        im.load(key, url, 128, Priority::High);
    } else if (it->second.state == Impl::State::Failed) {
        im.finishAccent(url, std::nullopt);
    }
}

void ImageCache::setBudget(size_t bytes) {
    impl().budget = bytes;
    impl().evictTo(bytes);
}

void ImageCache::trim(float keepFraction) {
    auto& im = impl();
    im.evictTo(static_cast<size_t>(static_cast<double>(im.used) * keepFraction));
}

void ImageCache::clear() {
    auto& im = impl();
    im.entries.clear();
    im.lru.clear();
    im.used = 0;
}

size_t ImageCache::memoryBytes() const { return impl().used; }

void ImageCache::pruneDisk(size_t maxBytes) {
    std::error_code ec;
    struct F {
        fs::path p;
        fs::file_time_type t;
        uintmax_t s;
    };
    std::vector<F> files;
    uintmax_t total = 0;
    for (const auto& de : fs::directory_iterator(paths::imageCacheDir(), ec)) {
        if (!de.is_regular_file(ec)) continue;
        F f{de.path(), de.last_write_time(ec), de.file_size(ec)};
        total += f.s;
        files.push_back(std::move(f));
    }
    if (total <= maxBytes) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
    for (const auto& f : files) {
        if (total <= maxBytes * 8 / 10) break;
        fs::remove(f.p, ec);
        total -= f.s;
    }
}

// ---------------------------------------------------------------------------------------------------

std::optional<Color> extractAccent(const uint8_t* px, int w, int h, int stride) {
    // Quantize to 4 bits/channel (4096 bins) over a sparse sample grid.
    struct Bin {
        uint32_t n = 0;
        uint32_t r = 0, g = 0, b = 0;
    };
    std::vector<Bin> bins(4096);
    const int step = std::max(1, std::min(w, h) / 48);
    for (int y = 0; y < h; y += step) {
        const uint8_t* row = px + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; x += step) {
            const uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            auto& bin = bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
            ++bin.n;
            bin.r += r;
            bin.g += g;
            bin.b += b;
        }
    }
    float bestScore = 0;
    Color best;
    for (const auto& bin : bins) {
        if (bin.n == 0) continue;
        const Color c{bin.r / (255.f * bin.n), bin.g / (255.f * bin.n), bin.b / (255.f * bin.n), 1};
        const Hsl hsl = toHsl(c);
        if (hsl.l < 0.12f || hsl.l > 0.92f) continue;
        const float score = static_cast<float>(bin.n) * hsl.s * hsl.s;
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }
    Hsl hsl = toHsl(best);
    if (bestScore <= 0 || hsl.s < 0.12f) return std::nullopt;   // grayscale art -> keep default accent
    hsl.s = std::max(hsl.s, 0.55f);
    hsl.l = std::clamp(hsl.l, 0.55f, 0.80f);
    return fromHsl(hsl);
}

} // namespace st::gfx
