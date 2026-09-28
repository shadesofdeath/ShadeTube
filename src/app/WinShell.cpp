// Windows shell integration: app identity, taskbar thumbnail toolbar, jump list and `--command`. See WinShell.h.
#include "app/WinShell.h"

#include "app/Installer.h"
#include "core/Async.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Resources.h"
#include "core/Utf.h"

#include <d2d1_3.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
// PKEY_AppUserModel_* / PKEY_Title are only declared unless INITGUID is set where propkey.h is first included.
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace st::app {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace winshell {

namespace {

constexpr const char* kTag = "winshell";

// Thumbnail toolbar button ids (WM_COMMAND / THBN_CLICKED).
constexpr UINT kIdPrevious = 0x5400, kIdPlayPause = 0x5401, kIdNext = 0x5402;
// Glyphs of the toolbar icon set, in icons_ order.
constexpr const char* kThumbGlyphs[4] = {"prev", "play", "pause", "next"};
// Jump-list task icon sizes: small icons at 100-200 % scale (the jump list picks the closest).
constexpr int kTaskIconSizes[] = {16, 20, 24, 32, 48};

struct CommandName {
    Command command;
    const wchar_t* name;
};
constexpr CommandName kCommands[] = {
    {Command::PlayPause, L"play-pause"}, {Command::Next, L"next"}, {Command::Previous, L"previous"}, {Command::Mini, L"mini"}};

// Last Windows mode the jump list was built for (-1 = not built): WM_SETTINGCHANGE arrives in bursts.
std::atomic<int> g_jumpListLight{-1};

// COM for shell / WIC objects on whatever thread we are on (the UI thread already has an STA: S_FALSE, balanced).
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComScope() = default;
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::string hex(HRESULT hr) { return std::format("0x{:08X}", static_cast<uint32_t>(hr)); }

HRESULT setString(IPropertyStore* store, const PROPERTYKEY& key, const std::wstring& value) {
    PROPVARIANT pv;
    HRESULT hr = InitPropVariantFromString(value.c_str(), &pv);
    if (FAILED(hr)) return hr;
    hr = store->SetValue(key, pv);
    PropVariantClear(&pv);
    return hr;
}

HRESULT setEmpty(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    return store->SetValue(key, pv);
}

// PNG bytes of a WIC bitmap source.
std::vector<uint8_t> encodePng(IWICImagingFactory* wic, IWICBitmapSource* source) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (!stream || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(source, nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()))
        return {};
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 16u * 1024 * 1024) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    const LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size())
        return {};
    return bytes;
}

// Task icon file for a command and Windows mode: the mode is in the name, so the shell's icon cache never shows the
// other mode's icon after a switch.
fs::path taskIconPath(Command c, bool light) {
    return installer::shellDir() / (std::wstring(L"task-") + commandName(c) + (light ? L"-light.ico" : L"-dark.ico"));
}

} // namespace

// ---- Commands ---------------------------------------------------------------------------------------------------

Command parseCommand(std::wstring_view name) {
    for (const auto& k : kCommands)
        if (CompareStringOrdinal(name.data(), static_cast<int>(name.size()), k.name, -1, TRUE) == CSTR_EQUAL) return k.command;
    return Command::None;
}

const wchar_t* commandName(Command c) {
    for (const auto& k : kCommands)
        if (k.command == c) return k.name;
    return L"";
}

Command commandFromArgs(const wchar_t* commandLine) {
    int argc = 0;
    LPWSTR* argv = commandLine && *commandLine ? CommandLineToArgvW(commandLine, &argc) : nullptr;
    Command c = Command::None;
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (CompareStringOrdinal(argv[i], -1, L"--command", -1, TRUE) == CSTR_EQUAL) {
            c = parseCommand(argv[i + 1]);
            break;
        }
    if (argv) LocalFree(argv);
    return c;
}

bool forwardCommand(Command c, const wchar_t* windowClass) {
    if (c == Command::None) return false;
    // FindWindow also finds hidden windows (tray / mini player). The main window is titled exactly "ShadeTube".
    HWND target = FindWindowW(windowClass, L"ShadeTube");
    if (!target) target = FindWindowW(windowClass, nullptr);
    const UINT msg = RegisterWindowMessageW(kCommandMessage);
    if (!target || !msg) return false;
    if (c == Command::Mini) {
        // This launch owns the foreground right (the user clicked the task): the mini player may take it.
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        if (pid) AllowSetForegroundWindow(pid);
    }
    return PostMessageW(target, msg, static_cast<WPARAM>(c), 0) != FALSE;
}

// ---- Identity ---------------------------------------------------------------------------------------------------

bool setProcessAppId() {
    if (!registrationAllowed()) return false;
    return SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(installer::appUserModelId().c_str()));
}

void setWindowIdentity(HWND hwnd) {
    if (!registrationAllowed()) return;
    ComPtr<IPropertyStore> store;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store));
    if (FAILED(hr)) {
        ST_LOG_WARN(kTag, "window property store unavailable ({})", hex(hr));
        return;
    }
    const std::wstring exe = currentExe().wstring();
    // Pinning this window's button relaunches the exe with the app's name and icon (resource 1).
    hr = setString(store.Get(), PKEY_AppUserModel_ID, installer::appUserModelId());
    if (SUCCEEDED(hr)) hr = setString(store.Get(), PKEY_AppUserModel_RelaunchCommand, L"\"" + exe + L"\"");
    if (SUCCEEDED(hr)) hr = setString(store.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource, L"ShadeTube");
    if (SUCCEEDED(hr)) hr = setString(store.Get(), PKEY_AppUserModel_RelaunchIconResource, exe + L",-1");
    if (FAILED(hr)) ST_LOG_WARN(kTag, "window identity not set ({})", hex(hr));
}

void clearWindowIdentity(HWND hwnd) {
    ComPtr<IPropertyStore> store;
    if (!hwnd || !registrationAllowed() || FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return;
    setEmpty(store.Get(), PKEY_AppUserModel_RelaunchIconResource);
    setEmpty(store.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource);
    setEmpty(store.Get(), PKEY_AppUserModel_RelaunchCommand);
    setEmpty(store.Get(), PKEY_AppUserModel_ID);
}

bool registrationAllowed() {
    const bool sandbox = GetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", nullptr, 0) > 0;
    return !sandbox || installer::appUserModelIdOverridden();
}

fs::path currentExe() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
}

std::vector<uint8_t> appIconPng() {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC group = FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON);
    const HGLOBAL groupData = group ? LoadResource(module, group) : nullptr;
    auto* dir = groupData ? static_cast<BYTE*>(LockResource(groupData)) : nullptr;
    const int id = dir ? LookupIconIdFromDirectoryEx(dir, TRUE, 256, 256, LR_DEFAULTCOLOR) : 0;
    const HRSRC icon = id ? FindResourceW(module, MAKEINTRESOURCEW(id), RT_ICON) : nullptr;
    const HGLOBAL iconData = icon ? LoadResource(module, icon) : nullptr;
    auto* data = iconData ? static_cast<BYTE*>(LockResource(iconData)) : nullptr;
    const DWORD size = icon ? SizeofResource(module, icon) : 0;
    if (!data || size == 0) return {};
    static constexpr uint8_t kPng[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size > sizeof kPng && std::equal(std::begin(kPng), std::end(kPng), data)) return {data, data + size};
    // A BMP image (the icon file changed): through an HICON and WIC's PNG encoder.
    const HICON h = CreateIconFromResourceEx(data, size, TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
    if (!h) return {};
    std::vector<uint8_t> png;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(h, &bitmap)))
        png = encodePng(wic.Get(), bitmap.Get());
    DestroyIcon(h);
    return png;
}

bool writeIfChanged(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    if (fs::file_size(path, ec) == bytes.size() && !ec) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<uint8_t> current{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (current == bytes) return false;
    }
    fs::create_directories(path.parent_path(), ec);
    // Next to the target, then one rename: the shell never reads a half-written icon.
    const fs::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void registerIdentity(const fs::path& exe) {
    ComScope com;
    const fs::path png = installer::shellDir() / L"ShadeTube.png";
    const auto bytes = appIconPng();
    const bool iconWritten = !bytes.empty() && writeIfChanged(png, bytes);
    if (bytes.empty()) ST_LOG_WARN(kTag, "app icon not available as PNG");
    const bool keyWritten = installer::registerAppIdentity(png);
    const auto shortcut = installer::ensureStartShortcut(exe);
    static constexpr const char* kShortcut[] = {"unchanged", "given the AUMID", "created", "retargeted"};
    ST_LOG_INFO(kTag, "identity {}: registry {}, icon {}, Start menu shortcut {}", toUtf8(installer::appUserModelId()),
                keyWritten ? "written" : "unchanged", iconWritten ? "written" : "unchanged", kShortcut[static_cast<int>(shortcut)]);
}

// ---- Glyph icons ------------------------------------------------------------------------------------------------

bool systemUsesLightTheme() {
    DWORD value = 0, size = sizeof value;
    const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                    L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st == ERROR_SUCCESS && value != 0;   // no value: the dark taskbar of Windows 10's default mode
}

uint32_t glyphColor(bool lightSurface) { return lightSurface ? 0x1A1A1A : 0xFFFFFF; }

std::vector<uint32_t> renderGlyph(std::string_view name, int px, uint32_t rgb) {
    if (px <= 0 || px > 256) return {};
    // The icon grid closest to the size (the same choice gfx::Icons makes), else the 24 px source scaled.
    const char* grid = px <= 16 ? "16" : px <= 20 ? "20" : "24";
    std::string_view svg = res::load(toWide(std::format("icons/{}/{}.svg", grid, name)));
    if (svg.empty()) svg = res::load(toWide(std::format("icons/24/{}.svg", name)));
    if (svg.empty()) return {};

    const UINT size = static_cast<UINT>(px);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DeviceContext5> dc;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())))
        return {};
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc))) return {};
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(svg.data()), static_cast<UINT>(svg.size())));
    ComPtr<ID2D1SvgDocument> doc;
    ComPtr<ID2D1SvgElement> root;
    const float fpx = static_cast<float>(px);
    if (!stream || FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(fpx, fpx), &doc))) return {};
    doc->GetRoot(&root);
    if (root) {   // let the viewBox scale the drawing to the requested size
        root->SetAttributeValue(L"width", fpx);
        root->SetAttributeValue(L"height", fpx);
    }
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawSvgDocument(doc.Get());
    if (FAILED(dc->EndDraw())) return {};

    // The glyphs are single-color (black) shapes: only the coverage matters, the color is ours.
    const WICRect all{0, 0, px, px};
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0, bytes = 0;
    BYTE* pixels = nullptr;
    if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, &lock)) || FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&bytes, &pixels)) || !pixels)
        return {};
    std::vector<uint32_t> argb(static_cast<size_t>(px) * px);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            const uint32_t a = pixels[static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4 + 3];
            argb[static_cast<size_t>(y) * px + x] = a ? (a << 24) | (rgb & 0xFFFFFF) : 0;
        }
    return argb;
}

HICON makeIcon(const std::vector<uint32_t>& argb, int px) {
    if (px <= 0 || argb.size() != static_cast<size_t>(px) * px) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, argb.data(), argb.size() * sizeof(uint32_t));
    // The alpha channel of a 32-bit color bitmap wins; the (all-zero, word-aligned rows) mask is still required.
    const std::vector<uint8_t> zeros(static_cast<size_t>((px + 15) / 16) * 2 * px, 0);
    const HBITMAP mask = CreateBitmap(px, px, 1, 1, zeros.data());
    ICONINFO info{TRUE, 0, 0, mask, color};
    const HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

std::vector<uint8_t> icoFile(const std::vector<IcoImage>& images) {
    std::vector<uint8_t> out;
    const auto put16 = [&](size_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    const auto put32 = [&](size_t v) {
        put16(v & 0xFFFF);
        put16((v >> 16) & 0xFFFF);
    };
    const auto maskStride = [](int px) { return static_cast<size_t>((px + 31) / 32) * 4; };
    const auto imageBytes = [&](const IcoImage& im) {
        return 40 + static_cast<size_t>(im.px) * im.px * 4 + maskStride(im.px) * static_cast<size_t>(im.px);
    };
    put16(0);   // ICONDIR
    put16(1);
    put16(images.size());
    size_t offset = 6 + 16 * images.size();
    for (const auto& im : images) {   // ICONDIRENTRY
        out.push_back(static_cast<uint8_t>(im.px >= 256 ? 0 : im.px));
        out.push_back(static_cast<uint8_t>(im.px >= 256 ? 0 : im.px));
        out.push_back(0);
        out.push_back(0);
        put16(1);    // planes
        put16(32);   // bits per pixel
        put32(imageBytes(im));
        put32(offset);
        offset += imageBytes(im);
    }
    for (const auto& im : images) {
        put32(40);   // BITMAPINFOHEADER: the height counts the color rows and the AND mask rows
        put32(static_cast<size_t>(im.px));
        put32(static_cast<size_t>(im.px) * 2);
        put16(1);
        put16(32);
        put32(BI_RGB);
        put32(static_cast<size_t>(im.px) * im.px * 4 + maskStride(im.px) * static_cast<size_t>(im.px));
        for (int i = 0; i < 4; ++i) put32(0);
        for (int y = im.px - 1; y >= 0; --y)   // bottom-up BGRA rows
            for (int x = 0; x < im.px; ++x) put32(im.argb[static_cast<size_t>(y) * im.px + x]);
        out.insert(out.end(), maskStride(im.px) * static_cast<size_t>(im.px), 0);   // AND mask: alpha decides
    }
    return out;
}

// ---- Jump list --------------------------------------------------------------------------------------------------

std::vector<JumpTask> jumpTasks() {
    return {
        {Command::PlayPause, tr(L"Oynat / Duraklat"), "play"},
        {Command::Next, tr(L"Sonraki"), "next"},
        {Command::Previous, tr(L"Önceki"), "prev"},
        {Command::Mini, tr(L"Mini oynatıcı"), "mini-player"},
    };
}

bool buildJumpList(const fs::path& exe, bool lightTheme) {
    ComScope com;
    const std::wstring appId = installer::appUserModelId();
    const auto tasks = jumpTasks();
    ComPtr<ICustomDestinationList> list;
    ComPtr<IObjectCollection> collection;
    HRESULT hr = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) hr = list->SetAppID(appId.c_str());
    UINT slots = 0;
    ComPtr<IObjectArray> removed;
    if (SUCCEEDED(hr)) hr = list->BeginList(&slots, IID_PPV_ARGS(&removed));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection));
    for (const auto& task : tasks) {
        if (FAILED(hr)) break;
        if (task.command == Command::Mini) {   // a line between playback and window tasks
            ComPtr<IShellLinkW> separator;
            ComPtr<IPropertyStore> props;
            PROPVARIANT pv;
            InitPropVariantFromBoolean(TRUE, &pv);
            hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&separator));
            if (SUCCEEDED(hr)) hr = separator.As(&props);
            if (SUCCEEDED(hr)) hr = props->SetValue(PKEY_AppUserModel_IsDestListSeparator, pv);
            if (SUCCEEDED(hr)) hr = props->Commit();
            if (SUCCEEDED(hr)) hr = collection->AddObject(separator.Get());
            if (FAILED(hr)) break;
        }
        // The icon file for this mode; the exe's own icon when it cannot be written.
        const fs::path icon = taskIconPath(task.command, lightTheme);
        std::vector<IcoImage> images;
        for (int px : kTaskIconSizes)
            if (auto argb = renderGlyph(task.glyph, px, glyphColor(lightTheme)); !argb.empty()) images.push_back({px, std::move(argb)});
        std::error_code ec;
        if (!images.empty()) writeIfChanged(icon, icoFile(images));
        const bool haveIcon = fs::exists(icon, ec);

        ComPtr<IShellLinkW> link;
        ComPtr<IPropertyStore> props;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (SUCCEEDED(hr)) hr = link->SetPath(exe.c_str());
        if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(exe.parent_path().c_str());
        if (SUCCEEDED(hr)) hr = link->SetArguments((std::wstring(L"--command ") + commandName(task.command)).c_str());
        if (SUCCEEDED(hr)) hr = link->SetIconLocation(haveIcon ? icon.c_str() : exe.c_str(), 0);
        if (SUCCEEDED(hr)) hr = link->SetDescription(task.title.c_str());
        if (SUCCEEDED(hr)) hr = link.As(&props);
        if (SUCCEEDED(hr)) hr = setString(props.Get(), PKEY_Title, task.title);
        if (SUCCEEDED(hr)) hr = props->Commit();
        if (SUCCEEDED(hr)) hr = collection->AddObject(link.Get());
    }
    ComPtr<IObjectArray> array;
    if (SUCCEEDED(hr)) hr = collection.As(&array);
    if (SUCCEEDED(hr)) hr = list->AddUserTasks(array.Get());
    if (SUCCEEDED(hr)) hr = list->CommitList();
    if (FAILED(hr)) {
        if (list) list->AbortList();
        ST_LOG_WARN(kTag, "jump list not built ({})", hex(hr));
        return false;
    }
    g_jumpListLight = lightTheme ? 1 : 0;
    ST_LOG_INFO(kTag, "jump list built: {} tasks ({} icons)", tasks.size(), lightTheme ? "light" : "dark");
    return true;
}

void refreshJumpList() {
    if (!registrationAllowed()) return;
    const int light = systemUsesLightTheme() ? 1 : 0;
    if (g_jumpListLight.exchange(light) == light) return;   // an accent-color change, or the rest of the burst
    background(Priority::Low, [exe = currentExe(), light] {
        try {
            buildJumpList(exe, light != 0);
        } catch (const std::exception& e) {
            ST_LOG_WARN(kTag, "jump list refresh failed: {}", e.what());
        }
    });
}

// ---- Taskbar thumbnail toolbar ----------------------------------------------------------------------------------

std::array<ThumbBar::Button, 3> ThumbBar::describe(const State& s) {
    return {{
        {kIdPrevious, Command::Previous, "prev", tr(L"Önceki"), s.hasTrack},
        {kIdPlayPause, Command::PlayPause, s.playing ? "pause" : "play", s.playing ? tr(L"Duraklat") : tr(L"Oynat"), s.hasTrack},
        {kIdNext, Command::Next, "next", tr(L"Sonraki"), s.hasTrack && s.canNext},
    }};
}

ThumbBar::ThumbBar(HWND hwnd) : hwnd_(hwnd) {
    buttonCreatedMsg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // An elevated ShadeTube would not receive Explorer's messages otherwise (UIPI).
    if (buttonCreatedMsg_) ChangeWindowMessageFilterEx(hwnd_, buttonCreatedMsg_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_COMMAND, MSGFLT_ALLOW, nullptr);
}

ThumbBar::~ThumbBar() {
    for (HICON icon : icons_)
        if (icon) DestroyIcon(icon);
}

bool ThumbBar::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (buttonCreatedMsg_ && msg == buttonCreatedMsg_) {
        add();
        return true;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == THBN_CLICKED) {
        for (const auto& b : describe(state_)) {
            if (b.id != LOWORD(wp)) continue;
            if (const auto fn = onCommand) {   // a copy: the handler may replace onCommand
                try {
                    fn(b.command);
                } catch (const std::exception& e) {
                    ST_LOG_ERROR(kTag, "thumbnail button handler threw: {}", e.what());
                }
            }
            return true;
        }
        return false;
    }
    if (msg == WM_DPICHANGED) renderIcons(LOWORD(wp));
    else if (msg == WM_SETTINGCHANGE && lp &&
             CompareStringOrdinal(reinterpret_cast<LPCWSTR>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
        renderIcons(GetDpiForWindow(hwnd_));
    return false;
}

Progress progressFor(const ProgressInput& in) {
    Progress p;
    if (!in.enabled || !in.hasTrack || in.live) return p;
    auto withValue = [&](TBPFLAG flag) {
        p.flag = flag;
        p.total = 1000;
        if (in.durationMs > 0)
            p.completed = static_cast<ULONGLONG>(std::clamp<int64_t>(in.positionMs * 1000 / in.durationMs, 0, 1000));
        return p;
    };
    switch (in.status) {
    case PlayStatus::Idle: break;
    case PlayStatus::Resolving:
    case PlayStatus::Buffering:
        // The first audio is on its way: a pulse. A stall later in the song keeps showing where it is.
        if (in.positionMs <= 0 || in.durationMs <= 0) p.flag = TBPF_INDETERMINATE;
        else withValue(TBPF_NORMAL);
        break;
    case PlayStatus::Playing:
        if (in.durationMs > 0) withValue(TBPF_NORMAL);
        break;
    case PlayStatus::Paused:
        if (in.durationMs > 0 && in.positionMs > 0) withValue(TBPF_PAUSED);
        break;
    case PlayStatus::Error:
        if (in.recentError) {
            withValue(TBPF_ERROR);
            if (in.durationMs <= 0 || p.completed == 0) p.completed = 1000;   // a full red bar says it best
        }
        break;
    }
    return p;
}

void ThumbBar::setProgress(const Progress& p) {
    if (p == progress_) return;
    const bool flagChanged = p.flag != progress_.flag;
    progress_ = p;
    if (!taskbar_) return;
    if (flagChanged) applyProgress();
    else if (p.flag == TBPF_NORMAL || p.flag == TBPF_PAUSED || p.flag == TBPF_ERROR)
        taskbar_->SetProgressValue(hwnd_, p.completed, p.total);
}

void ThumbBar::applyProgress() {
    if (!taskbar_) return;
    // SetProgressValue switches a NOPROGRESS / INDETERMINATE button to NORMAL: set the value first, then the state.
    if (progress_.flag == TBPF_NORMAL || progress_.flag == TBPF_PAUSED || progress_.flag == TBPF_ERROR)
        taskbar_->SetProgressValue(hwnd_, progress_.completed, progress_.total);
    taskbar_->SetProgressState(hwnd_, progress_.flag);
}

void ThumbBar::setState(const State& s) {
    if (s == state_) return;
    state_ = s;
    update();
}

std::array<THUMBBUTTON, 3> ThumbBar::buttons() const {
    std::array<THUMBBUTTON, 3> out{};
    const auto desc = describe(state_);
    for (size_t i = 0; i < desc.size(); ++i) {
        THUMBBUTTON& t = out[i];
        t.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        t.iId = desc[i].id;
        for (size_t g = 0; g < std::size(kThumbGlyphs); ++g)
            if (std::strcmp(kThumbGlyphs[g], desc[i].glyph) == 0) t.hIcon = icons_[g];
        wcsncpy_s(t.szTip, desc[i].tooltip, _TRUNCATE);
        t.dwFlags = desc[i].enabled ? THBF_ENABLED : THBF_DISABLED;
    }
    return out;
}

void ThumbBar::add() {
    added_ = false;
    taskbar_.Reset();
    ComPtr<ITaskbarList3> taskbar;
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
    if (SUCCEEDED(hr)) hr = taskbar->HrInit();
    if (FAILED(hr)) {
        ST_LOG_WARN(kTag, "taskbar unavailable ({})", hex(hr));
        return;
    }
    renderIcons(GetDpiForWindow(hwnd_));
    auto b = buttons();
    hr = taskbar->ThumbBarAddButtons(hwnd_, static_cast<UINT>(b.size()), b.data());
    const char* how = "added";
    if (FAILED(hr)) {   // this taskbar button already has the toolbar: only an update is allowed
        hr = taskbar->ThumbBarUpdateButtons(hwnd_, static_cast<UINT>(b.size()), b.data());
        how = "updated";
    }
    if (FAILED(hr)) {
        ST_LOG_WARN(kTag, "thumbnail toolbar not added ({})", hex(hr));
        taskbar_ = std::move(taskbar);   // the progress still works without the buttons
        applyProgress();
        return;
    }
    taskbar_ = std::move(taskbar);
    added_ = true;
    updateFailureLogged_ = false;
    applyProgress();
    ST_LOG_INFO(kTag, "thumbnail toolbar {}: {} buttons, {} px {} icons (playing {}, track {})", how, b.size(), iconPx_,
                iconLight_ ? "dark-on-light" : "light-on-dark", state_.playing, state_.hasTrack);
}

void ThumbBar::update() {
    if (!added_ || !taskbar_) return;
    auto b = buttons();
    const HRESULT hr = taskbar_->ThumbBarUpdateButtons(hwnd_, static_cast<UINT>(b.size()), b.data());
    if (FAILED(hr) && !updateFailureLogged_) {
        // Typically the button is gone (window hidden): the next TaskbarButtonCreated adds the toolbar again.
        updateFailureLogged_ = true;
        ST_LOG_WARN(kTag, "thumbnail toolbar update failed ({})", hex(hr));
    }
}

bool ThumbBar::renderIcons(UINT dpi) {
    const int px = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    const bool light = systemUsesLightTheme();
    if (px == iconPx_ && light == iconLight_ && icons_[0]) return false;
    std::array<HICON, 4> fresh{};
    for (size_t i = 0; i < fresh.size(); ++i) fresh[i] = makeIcon(renderGlyph(kThumbGlyphs[i], px, glyphColor(light)), px);
    if (!fresh[0]) ST_LOG_WARN(kTag, "thumbnail toolbar icons not rendered ({} px)", px);
    const auto old = icons_;
    icons_ = fresh;
    iconPx_ = px;
    iconLight_ = light;
    update();   // the taskbar drops the old handles here; only then are they destroyed
    for (HICON icon : old)
        if (icon) DestroyIcon(icon);
    return true;
}

} // namespace winshell

// ---- App startup ------------------------------------------------------------------------------------------------

void initWinShell() {
    using namespace winshell;
    if (!registrationAllowed()) {
        ST_LOG_INFO("winshell", "sandbox profile: AppUserModelId key, Start menu shortcut and jump list left alone");
        return;
    }
    const bool light = systemUsesLightTheme();
    g_jumpListLight = light ? 1 : 0;
    background(Priority::Low, [exe = currentExe(), light] {
        try {
            registerIdentity(exe);
            buildJumpList(exe, light);
        } catch (const std::exception& e) {
            ST_LOG_WARN("winshell", "shell registration failed: {}", e.what());
        }
    });
}

} // namespace st::app
