// Per-user install / uninstall: exe copy, Start menu shortcut, HKCU uninstall key, deferred folder removal, the
// relaunch after exit and the `--uninstall` command. See Installer.h.
#include "app/Installer.h"

#include "app/Autostart.h"
#include "app/Updater.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propvarutil.h>
#include <wrl/client.h>
// PKEY_AppUserModel_* are only declared unless INITGUID is set where propkey.h is first included.
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace st::app::installer {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ShadeTube";
constexpr wchar_t kWindowClass[] = L"ShadeTube.Window";   // ui::Window (main window and mini player)
constexpr wchar_t kMutexName[] = L"Local\\ShadeTube.SingleInstance";   // main.cpp's single-instance mutex
constexpr int kExitCancelled = 1602;   // ERROR_INSTALL_USEREXIT

// User-facing failures: the text in the UI language, thrown as UTF-8 (AboutSettings / the --uninstall report show it as is).
[[noreturn]] void fail(std::wstring_view message) { throw std::runtime_error(toUtf8(message)); }

std::wstring env(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring{};
}

// "Erişim engellendi" (the system message in the user's Windows language) for a Win32 error code.
std::wstring winError(DWORD code) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring text = n && msg ? std::wstring(msg, n) : i18n::format(tr(L"hata {}"), static_cast<long long>(code));
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
    return text;
}

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    return iequals(a.lexically_normal().wstring(), b.lexically_normal().wstring());
}

// Files ShadeTube itself puts in its folder: the exe, install()'s temp copy, the portable package's readme and the
// exact names the updater creates (ShadeTube.old.exe, ShadeTube.old-<n>.exe, ...). Anything else counts as a user's.
bool isOwnFile(const fs::path& name) {
    const std::wstring n = name.filename().wstring();
    for (const wchar_t* f : {L"ShadeTube.exe", L"ShadeTube.exe.tmp", L"BENIOKU.txt"})
        if (iequals(n, f)) return true;
    return updater::isUpdaterFileName(n);
}

// COM for the shell link on whatever thread we are on (the UI thread already has an STA: S_FALSE, balanced).
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComScope() = default;
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

// Sets System.AppUserModel.ID (and, for a shortcut the user did not ask for, "don't highlight as newly installed") in a
// shell link's property store. The caller saves the link.
HRESULT setLinkAppId(IShellLinkW* link, const std::wstring& appId, bool quiet) {
    ComPtr<IPropertyStore> props;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&props));
    PROPVARIANT pv;
    if (SUCCEEDED(hr)) hr = InitPropVariantFromString(appId.c_str(), &pv);
    if (FAILED(hr)) return hr;
    hr = props->SetValue(PKEY_AppUserModel_ID, pv);
    PropVariantClear(&pv);
    if (SUCCEEDED(hr) && quiet) {
        InitPropVariantFromBoolean(TRUE, &pv);
        hr = props->SetValue(PKEY_AppUserModel_ExcludeFromShowInNewInstall, pv);
        PropVariantClear(&pv);
    }
    if (SUCCEEDED(hr)) hr = props->Commit();
    return hr;
}

// `quiet`: a shortcut ShadeTube makes on its own (portable copy), see ensureStartShortcut(). Under a test AUMID with the
// real Start menu folder (leaveRealShortcut) the link gets no AUMID: the real app stamps its own on its next start.
void createShortcut(const fs::path& lnk, const fs::path& exe, bool quiet = false) {
    ComScope com;
    std::error_code ec;
    fs::create_directories(lnk.parent_path(), ec);
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(exe.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(exe.parent_path().c_str());
    if (SUCCEEDED(hr)) hr = link->SetDescription(tr(L"ShadeTube müzik çalar"));
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(exe.c_str(), 0);
    // The windows carry the same AppUserModelID: the taskbar pins / groups them with this shortcut, and the media
    // flyout takes the app's name and icon from it. Without it the shortcut still starts the app.
    if (SUCCEEDED(hr) && !leaveRealShortcut())
        if (const HRESULT id = setLinkAppId(link.Get(), appUserModelId(), quiet); FAILED(id))
            ST_LOG_WARN("install", "shortcut AppUserModelID not set (0x{:08X})", static_cast<unsigned>(id));
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(hr)) hr = link.As(&file);
    if (SUCCEEDED(hr)) hr = file->Save(lnk.c_str(), TRUE);
    if (FAILED(hr))
        fail(i18n::format(tr(L"Başlat menüsü kısayolu oluşturulamadı ({})."), {std::format(L"0x{:08X}", static_cast<unsigned>(hr))}));
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk.c_str(), nullptr);
}

// Adds the AUMID to an existing shortcut (installed by a version that did not set it). True when it carries it now.
bool stampShortcut(const fs::path& lnk) {
    ComScope com;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link.As(&file);
    if (SUCCEEDED(hr)) hr = file->Load(lnk.c_str(), STGM_READWRITE);
    if (SUCCEEDED(hr)) hr = setLinkAppId(link.Get(), appUserModelId(), false);
    if (SUCCEEDED(hr)) hr = file->Save(lnk.c_str(), TRUE);
    if (FAILED(hr)) {
        ST_LOG_WARN("install", "shortcut AppUserModelID not set (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, lnk.c_str(), nullptr);
    return true;
}

// Installer tests redirect the locations; unless they also give a test AUMID they must not touch the real identity.
bool testLocationsWithoutTestAppId() {
    const bool redirected = !env(L"SHADETUBE_INSTALL_DIR").empty() || !env(L"SHADETUBE_SHORTCUT_DIR").empty() ||
                            !env(L"SHADETUBE_UNINSTALL_KEY").empty();
    return redirected && !appUserModelIdOverridden();
}

std::wstring regString(HKEY key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

uint32_t regDword(HKEY key, const wchar_t* name) {
    DWORD value = 0, bytes = sizeof value;
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS ? value : 0;
}

// Starts a process detached from ours; outside our job object when that is allowed (a job that kills its processes
// on close, e.g. a terminal, would otherwise take the helper / the relaunched app down with us).
bool startDetached(std::wstring commandLine, const std::wstring& workDir, DWORD flags) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED)
        ok = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr, workDir.empty() ? nullptr : workDir.c_str(),
                            &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// The pending relaunch runs from this static's destructor, i.e. after wWinMain has returned (it closes the
// single-instance mutex first), so the new instance never meets the old one.
struct PendingLaunch {
    std::wstring commandLine, workDir;
    PendingLaunch() = default;
    PendingLaunch(const PendingLaunch&) = delete;
    PendingLaunch& operator=(const PendingLaunch&) = delete;
    ~PendingLaunch() {
        if (!commandLine.empty()) startDetached(commandLine, workDir, 0);
    }
};
PendingLaunch& pendingLaunch() {
    static PendingLaunch p;
    return p;
}

// What uninstall() removes now and what has to wait until this process has exited.
std::vector<fs::path> uninstallNow(bool removeUserData) {
    const Locations loc = locations();
    const auto installed = installedExe();
    const fs::path dir = installed ? installed->parent_path() : loc.dir;
    unregister();
    std::vector<fs::path> later;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        // The folder goes when it only holds ShadeTube's own files; otherwise only those files (never other data).
        std::vector<fs::path> own;
        bool foreign = false;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (isOwnFile(e.path())) own.push_back(e.path());
            else foreign = true;
        }
        const bool self = samePath(updater::currentExe().parent_path(), dir);
        if (!foreign) {
            if (!self) fs::remove_all(dir, ec);
            if (fs::exists(dir, ec)) later.push_back(dir);
        } else {
            for (const auto& f : own)
                if (!DeleteFileW(f.c_str())) later.push_back(f);
        }
    }
    if (removeUserData) {
        // The data folder is in use until the very end (log file, settings saved on exit): always after exit.
        const fs::path data = paths::appData();
        if (data.has_filename() && data.parent_path() != data.root_path()) later.push_back(data);
    }
    ST_LOG_INFO("install", "uninstalled from {} (user data {})", toUtf8(dir.wstring()), removeUserData ? "removed" : "kept");
    return later;
}

// ---- --uninstall dialogs (TaskDialog from comctl32 v6, message boxes as the fallback) ----------------------

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

// comctl32 v6 for this short-lived process: the exe's manifest does not ask for it, so a temporary activation
// context (from a manifest written to %TEMP%) does.
class Comctl6 {
public:
    Comctl6() {
        wchar_t tmp[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, tmp)) return;
        const fs::path manifest = fs::path(tmp) / std::format(L"ShadeTube-comctl6-{}.manifest", GetCurrentProcessId());
        std::ofstream(manifest) << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                                   "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><dependency>"
                                   "<dependentAssembly><assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" "
                                   "version=\"6.0.0.0\" processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
                                   "</dependentAssembly></dependency></assembly>";
        ACTCTXW act{};
        act.cbSize = sizeof act;
        act.lpSource = manifest.c_str();
        ctx_ = CreateActCtxW(&act);
        std::error_code ec;
        fs::remove(manifest, ec);
        if (ctx_ == INVALID_HANDLE_VALUE || !ActivateActCtx(ctx_, &cookie_)) return;
        active_ = true;
        module_ = LoadLibraryW(L"comctl32.dll");
        if (module_) taskDialog = reinterpret_cast<TaskDialogIndirectFn>(reinterpret_cast<void*>(GetProcAddress(module_, "TaskDialogIndirect")));
    }
    Comctl6(const Comctl6&) = delete;
    Comctl6& operator=(const Comctl6&) = delete;
    ~Comctl6() {
        if (module_) FreeLibrary(module_);
        if (active_) DeactivateActCtx(0, cookie_);
        if (ctx_ != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx_);
    }
    TaskDialogIndirectFn taskDialog = nullptr;

private:
    HANDLE ctx_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
    bool active_ = false;
    HMODULE module_ = nullptr;
};

TASKDIALOGCONFIG dialogConfig(const wchar_t* instruction) {
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    c.hInstance = GetModuleHandleW(nullptr);
    c.pszWindowTitle = L"ShadeTube";
    // The app icon (resource 1); TaskDialogIndirect fails outright when the resource is missing.
    c.pszMainIcon = FindResourceW(c.hInstance, MAKEINTRESOURCEW(1), RT_GROUP_ICON) ? MAKEINTRESOURCEW(1) : TD_INFORMATION_ICON;
    c.pszMainInstruction = instruction;
    return c;
}

bool showTaskDialog(const Comctl6& cc, const TASKDIALOGCONFIG& c, int* button, BOOL* verified) {
    if (!cc.taskDialog) return false;
    const HRESULT hr = cc.taskDialog(&c, button, nullptr, verified);
    if (FAILED(hr)) ST_LOG_WARN("install", "TaskDialogIndirect failed (0x{:08X})", static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

void message(const std::wstring& text, bool error) {
    Comctl6 cc;
    TASKDIALOGCONFIG c = dialogConfig(text.c_str());
    if (error) c.pszMainIcon = TD_ERROR_ICON;
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    if (showTaskDialog(cc, c, nullptr, nullptr)) return;
    MessageBoxW(nullptr, text.c_str(), L"ShadeTube", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

struct Choice {
    bool uninstall = false;
    bool removeData = false;
};

Choice confirmUninstall(const fs::path& dir, bool running, bool others) {
    // One paragraph per fact (each translated whole), blank lines between them.
    std::wstring content = i18n::format(tr(L"Program dosyaları, Başlat menüsü kısayolu ve Yüklü uygulamalar kaydı silinir. İndirdiğin "
                                           L"şarkılar silinmez.\n\nKonum: {}"),
                                        {dir.wstring()});
    if (running) content += std::wstring(L"\n\n") + tr(L"Açık olan ShadeTube kapatılacak.");
    if (others)
        content += std::wstring(L"\n\n") + tr(L"Ayarları da silmeyi seçersen başka bir klasörden açık olan ShadeTube da kapatılır.");
    const std::wstring instruction = tr(L"ShadeTube kaldırılsın mı?");
    Comctl6 cc;
    constexpr int kRemove = 100, kKeep = 101;
    const TASKDIALOG_BUTTON buttons[] = {{kRemove, tr(L"Uygulamayı kaldır")}, {kKeep, tr(L"Vazgeç")}};
    TASKDIALOGCONFIG c = dialogConfig(instruction.c_str());
    c.pszContent = content.c_str();
    c.cButtons = 2;
    c.pButtons = buttons;
    c.nDefaultButton = kRemove;
    c.pszVerificationText = tr(L"Ayarları, Spotify oturumunu, kitaplığı ve önbelleği de sil");
    int button = 0;
    BOOL verified = FALSE;
    if (showTaskDialog(cc, c, &button, &verified)) return {button == kRemove, verified != FALSE};
    if (MessageBoxW(nullptr, (instruction + L"\n\n" + content).c_str(), L"ShadeTube", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return {};
    const bool data = MessageBoxW(nullptr,
                                  tr(L"Ayarların, Spotify oturumun, kitaplığın ve önbellek de silinsin mi?\n\nHayır: bunlar bu "
                                     L"bilgisayarda kalır."),
                                  L"ShadeTube", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    return {true, data};
}

// A running ShadeTube: a process (not this one) with a window of ShadeTube's class, and its exe.
struct Instance {
    DWORD pid = 0, tid = 0;
    fs::path image;
};

std::vector<Instance> runningInstances() {
    struct Search {
        std::wstring cls;
        std::vector<Instance> found;
    } search{instanceClass(), {}};
    EnumWindows(
        [](HWND w, LPARAM param) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(param);
            wchar_t cls[128];
            if (!GetClassNameW(w, cls, 128) || s.cls != cls) return TRUE;
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() || std::any_of(s.found.begin(), s.found.end(), [&](const Instance& i) { return i.pid == pid; }))
                return TRUE;
            Instance inst{pid, tid, {}};
            if (const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t image[1024];
                DWORD n = 1024;
                if (QueryFullProcessImageNameW(p, 0, image, &n)) inst.image = std::wstring(image, n);
                CloseHandle(p);
            }
            s.found.push_back(std::move(inst));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// The single-instance mutex is held (by a ShadeTube that may have no window yet). Not meaningful inside the app,
// which holds it itself.
bool instanceMutexHeld() {
    if (const HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, instanceMutex().c_str())) {
        CloseHandle(m);
        return true;
    }
    return false;
}

// Asks those processes to quit (App::run ends on WM_QUIT and saves everything on the way out, like "Çıkış") and waits.
// False when one of them is still running (or could not be waited for).
bool closeInstances(const std::vector<Instance>& instances) {
    bool all = true;
    for (const auto& i : instances) {
        const HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, i.pid);
        PostThreadMessageW(i.tid, WM_QUIT, 0, 0);
        if (!proc) {
            all = false;
            continue;
        }
        if (WaitForSingleObject(proc, 30000) != WAIT_OBJECT_0) all = false;
        CloseHandle(proc);
    }
    return all;
}

} // namespace

std::wstring instanceClass() {
    const std::wstring c = env(L"SHADETUBE_INSTANCE_CLASS");
    return c.empty() ? std::wstring(kWindowClass) : c;
}

std::wstring instanceMutex() {
    const std::wstring m = env(L"SHADETUBE_INSTANCE_MUTEX");
    return m.empty() ? std::wstring(kMutexName) : m;
}

// ---- Locations / registration ---------------------------------------------------------------------------------

Locations locations() {
    Locations l;
    if (std::wstring d = env(L"SHADETUBE_INSTALL_DIR"); !d.empty()) {
        l.dir = d;
    } else {
        fs::path base = knownFolder(FOLDERID_UserProgramFiles);   // %LOCALAPPDATA%\Programs
        if (base.empty()) base = knownFolder(FOLDERID_LocalAppData) / L"Programs";
        l.dir = base / L"ShadeTube";
    }
    l.exe = l.dir / L"ShadeTube.exe";
    fs::path links = env(L"SHADETUBE_SHORTCUT_DIR");
    if (links.empty()) links = knownFolder(FOLDERID_Programs);
    l.shortcut = links / L"ShadeTube.lnk";
    l.uninstallKey = kUninstallKey;
    if (std::wstring k = env(L"SHADETUBE_UNINSTALL_KEY"); !k.empty()) {
        // Always HKCU: a hive prefix is stripped ("HKCU\Software\X" and "Software\X" mean the same key). A malformed
        // override never falls back to the real key.
        if (const auto slash = k.find(L'\\'); slash != std::wstring::npos && k.substr(0, 2) == L"HK") k.erase(0, slash + 1);
        while (!k.empty() && k.back() == L'\\') k.pop_back();
        l.uninstallKey = k.find(L'\\') != std::wstring::npos && k.find(L"\\\\") == std::wstring::npos
                             ? k
                             : L"Software\\ShadeTube\\InvalidUninstallKeyOverride";
    }
    return l;
}

Registration readRegistration() {
    Registration r;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, locations().uninstallKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return r;
    r.exists = true;
    r.displayName = regString(key, L"DisplayName");
    r.displayVersion = regString(key, L"DisplayVersion");
    r.publisher = regString(key, L"Publisher");
    r.displayIcon = regString(key, L"DisplayIcon");
    r.installLocation = regString(key, L"InstallLocation");
    r.uninstallString = regString(key, L"UninstallString");
    r.quietUninstallString = regString(key, L"QuietUninstallString");
    r.estimatedSizeKb = regDword(key, L"EstimatedSize");
    r.noModify = regDword(key, L"NoModify");
    r.noRepair = regDword(key, L"NoRepair");
    RegCloseKey(key);
    return r;
}

std::optional<fs::path> installedExe() {
    const Registration r = readRegistration();
    if (!r.exists || r.installLocation.empty()) return std::nullopt;
    const fs::path exe = fs::path(r.installLocation) / L"ShadeTube.exe";
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) return std::nullopt;
    return exe;
}

bool runningInstalled() {
    const auto exe = installedExe();
    return exe && samePath(updater::currentExe(), *exe);
}

void writeRegistration(const fs::path& exe) {
    const Locations loc = locations();
    HKEY key = nullptr;
    const LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, loc.uninstallKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                       nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) fail(i18n::format(tr(L"Uygulama kaydı yazılamadı ({})."), {winError(static_cast<DWORD>(st))}));
    const std::wstring quoted = L"\"" + exe.wstring() + L"\"";
    std::error_code ec;
    const uintmax_t bytes = fs::file_size(exe, ec);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    LSTATUS err = ERROR_SUCCESS;
    auto sz = [&](const wchar_t* name, const std::wstring& value) {
        const LSTATUS s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        if (s != ERROR_SUCCESS) err = s;
    };
    auto dword = [&](const wchar_t* name, DWORD value) {
        const LSTATUS s = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
        if (s != ERROR_SUCCESS) err = s;
    };
    sz(L"DisplayName", L"ShadeTube");
    sz(L"DisplayVersion", toWide(updater::normalizeVersion(updater::fileProductVersion(exe))));
    sz(L"Publisher", L"ShadeTube");
    sz(L"DisplayIcon", exe.wstring() + L",0");
    sz(L"InstallLocation", exe.parent_path().wstring());
    sz(L"UninstallString", quoted + L" --uninstall");
    sz(L"QuietUninstallString", quoted + L" --uninstall --quiet");
    sz(L"InstallDate", std::format(L"{:04}{:02}{:02}", now.wYear, now.wMonth, now.wDay));
    dword(L"EstimatedSize", static_cast<DWORD>(ec ? 0 : (bytes + 1023) / 1024));   // KB
    dword(L"NoModify", 1);
    dword(L"NoRepair", 1);
    RegCloseKey(key);
    if (err != ERROR_SUCCESS) fail(i18n::format(tr(L"Uygulama kaydı yazılamadı ({})."), {winError(static_cast<DWORD>(err))}));
}

void refreshRegistration() {
    try {
        const auto exe = installedExe();
        if (!exe || !samePath(updater::currentExe(), *exe)) return;
        const Registration r = readRegistration();
        const std::wstring version = toWide(updater::normalizeVersion(updater::fileProductVersion(*exe)));
        std::error_code ec;
        const auto kb = static_cast<uint32_t>((fs::file_size(*exe, ec) + 1023) / 1024);
        if (r.displayVersion == version && r.estimatedSizeKb == kb) return;
        writeRegistration(*exe);
        ST_LOG_INFO("install", "registration updated to {}", toUtf8(version));
    } catch (const std::exception& e) {
        ST_LOG_WARN("install", "registration refresh failed: {}", e.what());
    }
}

fs::path shortcutTarget(const fs::path& lnk) {
    ComScope com;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    wchar_t target[MAX_PATH]{};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
        FAILED(file->Load(lnk.c_str(), STGM_READ)) || FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)))
        return {};
    return target;
}

// ---- Install / uninstall --------------------------------------------------------------------------------------

void install(const fs::path& sourceExe) {
    const Locations loc = locations();
    std::error_code ec;
    fs::create_directories(loc.dir, ec);
    if (ec) fail(i18n::format(tr(L"Kurulum klasörü oluşturulamadı ({})."), {loc.dir.wstring()}));
    if (!samePath(sourceExe, loc.exe)) {
        // Copy next to the target first, then replace it in one rename: never a half-written ShadeTube.exe.
        const fs::path tmp = loc.dir / L"ShadeTube.exe.tmp";
        if (!CopyFileW(sourceExe.c_str(), tmp.c_str(), FALSE))
            fail(i18n::format(tr(L"ShadeTube kurulum klasörüne kopyalanamadı ({})."), {winError(GetLastError())}));
        if (!MoveFileExW(tmp.c_str(), loc.exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // An older installed exe that is still mapped (running in another session) cannot be overwritten, but it
            // can be renamed away (its ShadeTube.old.exe is removed by the installed copy's next start).
            try {
                updater::swapExe(loc.exe, tmp);
            } catch (const std::exception&) {
                DeleteFileW(tmp.c_str());
                throw;
            }
        }
    }
    writeRegistration(loc.exe);
    createShortcut(loc.shortcut, loc.exe);
    ST_LOG_INFO("install", "installed {} to {}", updater::fileProductVersion(loc.exe), toUtf8(loc.exe.wstring()));
}

void unregister() {
    const Locations loc = locations();
    if (DeleteFileW(loc.shortcut.c_str())) SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, loc.shortcut.c_str(), nullptr);
    // The identity (key, <data>\shell, jump list) is shared by every copy: another ShadeTube that keeps running (a
    // portable copy while the installed one goes) still uses it, and registers it again at its next start anyway.
    if (!testLocationsWithoutTestAppId()) {
        if (runningInstances().empty()) unregisterAppIdentity();
        else ST_LOG_INFO("install", "app identity kept: another ShadeTube is running");
    }
    // "Windows ile başlat": the Run value would start the removed exe at sign-in. (A portable copy that is still used
    // writes it again at its next start.) A test that redirects the locations never touches the real one.
    autostart::remove();
    // RegDeleteKeyW removes only this (value-only) key; locations() guarantees a non-empty sub key with a parent.
    const LSTATUS st = RegDeleteKeyW(HKEY_CURRENT_USER, loc.uninstallKey.c_str());
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND)
        fail(i18n::format(tr(L"Uygulama kaydı silinemedi ({})."), {winError(static_cast<DWORD>(st))}));
}

void uninstall(bool removeUserData) {
    const auto later = uninstallNow(removeUserData);
    if (!later.empty()) deleteAfterExit(later);
}

bool deleteAfterExit(const std::vector<fs::path>& paths) {
    // One retry loop per path: `if exist` -> rmdir / del -> ~1 s pause while it still exists, up to 60 times. The loop
    // starts at once; whatever this process still holds (the exe image, the log file) goes as soon as it has exited.
    // The paths never appear in the text cmd.exe parses (a '%' in a folder name would be expanded there, even quoted):
    // they are handed over as ST_DEL_<n> environment variables and read with delayed expansion (!ST_DEL_1!), which
    // runs after % expansion and FOR substitution and is not parsed again, so any character of a path stays literal.
    static std::mutex busy;   // the variables live in this process's environment until the helper has its copy
    std::lock_guard lock(busy);
    std::wstring script;
    std::vector<std::wstring> vars;
    for (const auto& p : paths) {
        std::error_code ec;
        const std::wstring var = L"ST_DEL_" + std::to_wstring(vars.size() + 1);
        if (!SetEnvironmentVariableW(var.c_str(), p.c_str())) continue;
        vars.push_back(var);
        const std::wstring q = L"\"!" + var + L"!\"";
        if (!script.empty()) script += L" & ";
        script += L"for /l %i in (1,1,60) do @if exist " + q + L" (" + (fs::is_directory(p, ec) ? L"rmdir /s /q " : L"del /f /q ") + q +
                  L" >nul 2>&1 & if exist " + q + L" ping -n 2 127.0.0.1 >nul)";
    }
    if (script.empty()) return paths.empty();
    wchar_t system[MAX_PATH];
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring systemDir = n > 0 && n < MAX_PATH ? std::wstring(system, n) : L"C:\\Windows\\System32";
    // /v:on: delayed expansion. /s: the outer quotes are stripped as a whole. The helper's working directory is
    // System32 so it never pins a folder it is about to delete.
    const std::wstring cmd = L"\"" + systemDir + L"\\cmd.exe\" /v:on /d /q /s /c \"" + script + L"\"";
    const bool started = startDetached(cmd, systemDir, CREATE_NO_WINDOW);
    const DWORD err = GetLastError();
    for (const auto& v : vars) SetEnvironmentVariableW(v.c_str(), nullptr);   // the helper has its own copy now
    if (started) {
        ST_LOG_INFO("install", "deferred delete of {} path(s)", paths.size());
        return true;
    }
    ST_LOG_ERROR("install", "deferred delete helper failed: {}", toUtf8(winError(err)));
    for (const auto& p : paths) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);   // needs admin; best effort
    return false;
}

void launchAfterExit(const fs::path& exe, std::wstring args) {
    auto& p = pendingLaunch();
    p.commandLine = L"\"" + exe.wstring() + L"\"" + (args.empty() ? L"" : L" " + args);
    p.workDir = exe.parent_path().wstring();
    AllowSetForegroundWindow(ASFW_ANY);   // this click owns the foreground right: the new instance may take it
    ST_LOG_INFO("install", "will start {} after exit", toUtf8(p.commandLine));
}

bool hasArg(std::wstring_view flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; ++i) found = iequals(argv[i], flag);
    LocalFree(argv);
    return found;
}

bool uninstallRequested() { return hasArg(L"--uninstall"); }

bool otherInstanceRunning() { return !runningInstances().empty(); }

int runUninstallCommand() {
    const bool quiet = hasArg(L"--quiet") || hasArg(L"/quiet") || hasArg(L"/S");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    log::init();
    ST_LOG_INFO("install", "uninstall command{}", quiet ? " (quiet)" : "");
    int code = 0;
    const Locations loc = locations();
    const auto installed = installedExe();
    std::error_code ec;
    if (!installed && !readRegistration().exists && !fs::exists(loc.exe, ec) && !fs::exists(loc.shortcut, ec)) {
        if (!quiet) message(tr(L"ShadeTube bu bilgisayarda kurulu değil."), false);
    } else {
        const fs::path dir = installed ? installed->parent_path() : loc.dir;
        // The copy running from the install folder keeps it in use and has to go; copies running from anywhere else
        // (portable, dev) only matter when the shared data folder is deleted too.
        std::vector<Instance> inDir, others;
        for (auto& i : runningInstances()) (!i.image.empty() && samePath(i.image.parent_path(), dir) ? inDir : others).push_back(std::move(i));
        Choice choice{true, false};   // quiet: uninstall, keep the user data
        if (!quiet) choice = confirmUninstall(dir, !inDir.empty(), !others.empty());
        if (!choice.uninstall) {
            code = kExitCancelled;
        } else if (!closeInstances(inDir)) {
            ST_LOG_WARN("install", "the running instance did not exit");
            if (!quiet) message(tr(L"ShadeTube kapatılamadı. Uygulamayı kapatıp yeniden dene."), true);
            code = 1;
        } else {
            // Never delete the data folder under a running ShadeTube: close the other copies first; if one stays (or
            // the single-instance mutex is still held by a copy without a window), keep the data.
            bool keptData = false;
            if (choice.removeData && (!closeInstances(others) || instanceMutexHeld())) {
                ST_LOG_WARN("install", "another ShadeTube is still running: user data kept");
                choice.removeData = false;
                keptData = true;
            }
            try {
                const auto later = uninstallNow(choice.removeData);
                // Reported before the files go: the helper's retry loop then starts right before this process exits.
                if (!quiet)
                    message(keptData ? tr(L"Program kaldırıldı; açık bir ShadeTube kapatılamadığı için ayarlar ve veriler silinmedi.")
                                     : tr(L"ShadeTube kaldırıldı."),
                            false);
                if (!later.empty()) deleteAfterExit(later);
            } catch (const std::exception& e) {
                ST_LOG_ERROR("install", "uninstall failed: {}", e.what());
                if (!quiet) message(i18n::format(tr(L"ShadeTube kaldırılamadı: {}"), {toWide(e.what())}), true);
                code = 1;
            }
        }
    }
    log::shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}

// ---- App identity ---------------------------------------------------------------------------------------------

std::wstring appUserModelId() {
    const std::wstring id = env(L"SHADETUBE_AUMID");
    if (id.empty()) return kAppUserModelId;
    // An AUMID is at most 128 characters without spaces; it also names a registry key here, so no backslash either.
    const bool valid = id.size() <= 128 && id.find_first_of(L" \\/\t") == std::wstring::npos;
    return valid ? id : std::wstring(kAppUserModelId) + L".InvalidOverride";
}

bool appUserModelIdOverridden() { return !env(L"SHADETUBE_AUMID").empty(); }

bool leaveRealShortcut() { return appUserModelIdOverridden() && env(L"SHADETUBE_SHORTCUT_DIR").empty(); }

std::wstring appIdKey() { return L"Software\\Classes\\AppUserModelId\\" + appUserModelId(); }

fs::path shellDir() { return paths::appData() / L"shell"; }

AppIdentity readAppIdentity() {
    AppIdentity a;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, appIdKey().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return a;
    a.exists = true;
    a.displayName = regString(key, L"DisplayName");
    a.iconUri = regString(key, L"IconUri");
    RegCloseKey(key);
    return a;
}

bool registerAppIdentity(const fs::path& iconPng) {
    const AppIdentity now = readAppIdentity();
    const std::wstring icon = iconPng.wstring();
    if (now.exists && now.displayName == L"ShadeTube" && now.iconUri == icon) return false;
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, appIdKey().c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                 &key, nullptr);
    if (st != ERROR_SUCCESS) {
        ST_LOG_WARN("identity", "AppUserModelId key not written: {}", toUtf8(winError(static_cast<DWORD>(st))));
        return false;
    }
    auto sz = [&](const wchar_t* name, const std::wstring& value) {
        const LSTATUS s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        if (s != ERROR_SUCCESS) st = s;
    };
    sz(L"DisplayName", L"ShadeTube");   // brand name, never translated
    sz(L"IconUri", icon);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) ST_LOG_WARN("identity", "AppUserModelId values not written: {}", toUtf8(winError(static_cast<DWORD>(st))));
    return st == ERROR_SUCCESS;
}

void unregisterAppIdentity() {
    // RegDeleteTreeW on exactly our key (appUserModelId() is never empty and has no backslash).
    const LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, appIdKey().c_str());
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND)
        ST_LOG_WARN("identity", "AppUserModelId key not removed: {}", toUtf8(winError(static_cast<DWORD>(st))));
    std::error_code ec;
    fs::remove_all(shellDir(), ec);
    ComScope com;
    ComPtr<ICustomDestinationList> list;
    if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
        list->DeleteList(appUserModelId().c_str());
    ST_LOG_INFO("identity", "app identity removed ({})", toUtf8(appUserModelId()));
}

std::wstring shortcutAppId(const fs::path& lnk) {
    ComScope com;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    ComPtr<IPropertyStore> props;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
        FAILED(file->Load(lnk.c_str(), STGM_READ)) || FAILED(link.As(&props)))
        return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring id;
    if (SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) id = pv.pwszVal;
    PropVariantClear(&pv);
    return id;
}

ShortcutAction ensureStartShortcut(const fs::path& exe) {
    if (leaveRealShortcut()) return ShortcutAction::None;   // a test AUMID never goes into the user's real shortcut
    const Locations loc = locations();
    const fs::path marker = shellDir() / L"start-shortcut.flag";
    std::error_code ec;
    const bool exists = fs::exists(loc.shortcut, ec);
    const fs::path target = exists ? shortcutTarget(loc.shortcut) : fs::path{};
    const bool stamped = exists && shortcutAppId(loc.shortcut) == appUserModelId();
    const auto stamp = [&] { return !stamped && stampShortcut(loc.shortcut) ? ShortcutAction::Stamped : ShortcutAction::None; };
    try {
        if (const auto installed = installedExe()) {
            // The installer's shortcut: an older version created it without the AUMID.
            return exists && !target.empty() && samePath(target, *installed) ? stamp() : ShortcutAction::None;
        }
        if (!exists) {
            if (fs::exists(marker, ec)) return ShortcutAction::None;   // created once, removed by the user since
            createShortcut(loc.shortcut, exe, true);
            fs::create_directories(marker.parent_path(), ec);
            std::ofstream(marker) << "1";
            ST_LOG_INFO("identity", "Start menu shortcut created for {}", toUtf8(exe.wstring()));
            return ShortcutAction::Created;
        }
        if (target.empty()) return ShortcutAction::None;   // unreadable: not ours to rewrite
        if (samePath(target, exe) || fs::exists(target, ec)) return stamp();
        createShortcut(loc.shortcut, exe, true);   // its portable copy was moved or deleted: point it at this one
        ST_LOG_INFO("identity", "Start menu shortcut now points at {}", toUtf8(exe.wstring()));
        return ShortcutAction::Retargeted;
    } catch (const std::exception& e) {
        ST_LOG_WARN("identity", "Start menu shortcut not written: {}", e.what());
        return ShortcutAction::None;
    }
}

} // namespace st::app::installer
