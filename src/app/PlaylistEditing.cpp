#include "app/PlaylistEditing.h"

#include "app/CatalogJson.h"
#include "core/Utf.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace st::app::pledit {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ---- Reordering ------------------------------------------------------------------------------------------------

namespace {
std::vector<char> pickedMask(int count, const std::vector<int>& picked) {
    std::vector<char> mask(static_cast<size_t>(std::max(count, 0)), 0);
    for (int i : picked)
        if (i >= 0 && i < count) mask[static_cast<size_t>(i)] = 1;
    return mask;
}
} // namespace

std::vector<int> moveOrder(int count, const std::vector<int>& picked, int insertBefore) {
    count = std::max(count, 0);
    insertBefore = std::clamp(insertBefore, 0, count);
    const auto mask = pickedMask(count, picked);
    std::vector<int> out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < insertBefore; ++i)
        if (!mask[static_cast<size_t>(i)]) out.push_back(i);
    for (int i = 0; i < count; ++i)
        if (mask[static_cast<size_t>(i)]) out.push_back(i);
    for (int i = insertBefore; i < count; ++i)
        if (!mask[static_cast<size_t>(i)]) out.push_back(i);
    return out;
}

bool isIdentity(const std::vector<int>& order) {
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != static_cast<int>(i)) return false;
    return true;
}

std::vector<int> newPositions(const std::vector<int>& order, const std::vector<int>& picked) {
    const int count = static_cast<int>(order.size());
    const auto mask = pickedMask(count, picked);
    std::vector<int> out;
    for (int k = 0; k < count; ++k)
        if (order[static_cast<size_t>(k)] >= 0 && order[static_cast<size_t>(k)] < count &&
            mask[static_cast<size_t>(order[static_cast<size_t>(k)])])
            out.push_back(k);
    return out;
}

Anchor moveAnchor(int count, const std::vector<int>& picked, int insertBefore) {
    count = std::max(count, 0);
    insertBefore = std::clamp(insertBefore, 0, count);
    const auto mask = pickedMask(count, picked);
    for (int i = insertBefore; i < count; ++i)
        if (!mask[static_cast<size_t>(i)]) return {Anchor::Kind::Before, i};
    for (int i = insertBefore - 1; i >= 0; --i)
        if (!mask[static_cast<size_t>(i)]) return {Anchor::Kind::After, i};
    return {};
}

std::vector<Run> restoreRuns(int count, const std::vector<int>& removed) {
    count = std::max(count, 0);
    const auto mask = pickedMask(count, removed);
    std::vector<Run> out;
    for (int i = 0; i < count;) {
        if (!mask[static_cast<size_t>(i)]) {
            ++i;
            continue;
        }
        Run run;
        const int first = i;
        while (i < count && mask[static_cast<size_t>(i)]) run.rows.push_back(i++);
        if (i < count) run.anchor = {Anchor::Kind::Before, i};   // the row right after the run stayed
        else if (first > 0) run.anchor = {Anchor::Kind::After, first - 1};
        out.push_back(std::move(run));
    }
    return out;
}

// ---- Cover images ----------------------------------------------------------------------------------------------

namespace {

// COM for the calling thread (MTA); a thread that already has COM keeps it (and we don't uninitialize it).
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    return wic;
}

ComPtr<IWICBitmapFrameDecode> decodeFirstFrame(IWICImagingFactory* wic, const uint8_t* data, size_t size) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!data || size == 0 || size > 0xFFFFFFFFu || FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;
    return frame;
}

// The EXIF orientation of a photo (JPEG / TIFF / HEIF metadata) as the transform that shows it upright.
WICBitmapTransformOptions uprightTransform(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return WICBitmapTransformRotate0;
    int orientation = 1;
    for (const wchar_t* q : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetMetadataByName(q, &v)) && v.vt == VT_UI2) orientation = v.uiVal;
        PropVariantClear(&v);
        if (orientation != 1) break;
    }
    switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

// The centered square of `frame`, `side` pixels, upright, as 24-bit BGR (transparency over black, like the dark
// placeholder it replaces).
ComPtr<IWICBitmap> squareBitmap(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame, int side) {
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return nullptr;
    const UINT s = std::min(w, h);
    const WICRect crop{static_cast<INT>((w - s) / 2), static_cast<INT>((h - s) / 2), static_cast<INT>(s), static_cast<INT>(s)};
    ComPtr<IWICBitmapClipper> clipper;
    if (FAILED(wic->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(frame, &crop))) return nullptr;
    ComPtr<IWICBitmapSource> src = clipper;
    const UINT out = std::min(s, static_cast<UINT>(std::max(side, 1)));
    if (out != s) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(src.Get(), out, out, WICBitmapInterpolationModeHighQualityCubic)))
            return nullptr;
        src = scaler;
    }
    if (const auto t = uprightTransform(frame); t != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(src.Get(), t))) src = rot;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    // Premultiplied color without its alpha = the image composed over black.
    std::vector<BYTE> bgra(static_cast<size_t>(out) * out * 4);
    if (FAILED(conv->CopyPixels(nullptr, out * 4, static_cast<UINT>(bgra.size()), bgra.data()))) return nullptr;
    std::vector<BYTE> bgr(static_cast<size_t>(out) * out * 3);
    for (size_t i = 0, j = 0; i < bgra.size(); i += 4, j += 3) {
        bgr[j] = bgra[i];
        bgr[j + 1] = bgra[i + 1];
        bgr[j + 2] = bgra[i + 2];
    }
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromMemory(out, out, GUID_WICPixelFormat24bppBGR, out * 3, static_cast<UINT>(bgr.size()),
                                           bgr.data(), &bmp)))
        return nullptr;
    return bmp;
}

std::vector<uint8_t> encodeJpeg(IWICImagingFactory* wic, IWICBitmapSource* src, float quality) {
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &props)))
        return {};
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = quality;
    props->Write(1, &opt, &v);
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)) ||
        FAILED(frame->WriteSource(src, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 64u * 1024 * 1024) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    const LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size())
        return {};
    return bytes;
}

} // namespace

Cover squareJpeg(const uint8_t* data, size_t size, int maxSide, size_t maxBytes) {
    ComScope com;
    const auto wic = factory();
    if (!wic) return {};
    const auto frame = decodeFirstFrame(wic.Get(), data, size);
    if (!frame) return {};
    // Quality steps down first (a photo fits at the first or second step); a noisy image also gets smaller.
    for (int side = std::max(maxSide, 1);; side = side * 4 / 5) {
        const auto bmp = squareBitmap(wic.Get(), frame.Get(), side);
        if (!bmp) return {};
        UINT w = 0, h = 0;
        bmp->GetSize(&w, &h);
        for (const float q : {0.9f, 0.82f, 0.74f, 0.65f, 0.55f}) {
            auto jpeg = encodeJpeg(wic.Get(), bmp.Get(), q);
            if (jpeg.empty()) return {};
            if (jpeg.size() <= maxBytes) return {std::move(jpeg), static_cast<int>(w)};
        }
        if (side <= 64) return {};
    }
}

Cover squareJpeg(const fs::path& file, int maxSide, size_t maxBytes) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto len = static_cast<long long>(f.tellg());
    if (len <= 0 || len > 64ll * 1024 * 1024) return {};   // no picture we'd take is that big
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), len)) return {};
    return squareJpeg(bytes.data(), bytes.size(), maxSide, maxBytes);
}

std::pair<int, int> imageSize(const uint8_t* data, size_t size) {
    ComScope com;
    const auto wic = factory();
    if (!wic) return {0, 0};
    const auto frame = decodeFirstFrame(wic.Get(), data, size);
    UINT w = 0, h = 0;
    if (!frame || FAILED(frame->GetSize(&w, &h))) return {0, 0};
    return {static_cast<int>(w), static_cast<int>(h)};
}

// ---- Local playlists -------------------------------------------------------------------------------------------

nlohmann::json toJson(const LocalPlaylist& p) {
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& t : p.tracks) tracks.push_back(app::toJson(t));
    nlohmann::json j{{"id", p.meta.id}, {"n", p.meta.name}, {"desc", p.meta.description}, {"c", p.meta.createdAt},
                     {"tracks", std::move(tracks)}};
    if (!p.cover.empty()) j["cover"] = toUtf8(p.cover);
    return j;
}

bool fromJson(const nlohmann::json& j, LocalPlaylist& out) {
    out = {};
    if (!j.is_object()) return false;
    out.meta.id = j.value("id", "");
    if (out.meta.id.empty()) return false;
    out.meta.name = j.value("n", "");
    out.meta.description = j.value("desc", "");
    out.meta.createdAt = j.value("c", int64_t{0});
    out.cover = toWide(j.value("cover", ""));
    if (auto t = j.find("tracks"); t != j.end() && t->is_array())
        for (const auto& x : *t) out.tracks.push_back(trackFromJson(x));
    return true;
}

std::wstring saveCover(const fs::path& dir, const std::string& playlistId, const std::vector<uint8_t>& jpeg) {
    if (jpeg.empty() || playlistId.empty()) return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::wstring name;
    for (const char c : playlistId)   // "local:0123abcd" -> "local-0123abcd"
        name += (std::isalnum(static_cast<unsigned char>(c)) ? static_cast<wchar_t>(c) : L'-');
    static std::atomic<unsigned> seq{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const fs::path file = dir / (name + L"-" + std::to_wstring(ms) + L"-" + std::to_wstring(seq++) + L".jpg");
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return {};
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return {};
    }
    return file.wstring();
}

void deleteCover(const fs::path& dir, const std::wstring& file) {
    if (file.empty()) return;
    std::error_code ec;
    const fs::path p(file);
    if (!fs::equivalent(p.parent_path(), dir, ec) || ec) return;
    fs::remove(p, ec);
}

} // namespace st::app::pledit
