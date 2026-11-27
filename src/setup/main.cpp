// ShadeTubeSetup.exe: per-user install / update / uninstall of ShadeTube with its own window (SetupUi) or silently
// (/S, package managers). See docs/DEVELOPMENT.md "Setup".
#include "app/Installer.h"
#include "app/Updater.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/Text.h"
#include "setup/Payload.h"
#include "setup/SetupArgs.h"
#include "setup/SetupEngine.h"
#include "setup/SetupUi.h"

#include <windows.h>

#include <string>

using namespace st;
using namespace st::setup;
namespace installer = st::app::installer;

namespace {

// The switches, for /? and a command line that could not be read.
void usage(const std::wstring& problem) {
    std::wstring text = problem.empty() ? std::wstring() : i18n::format(tr(L"Bilinmeyen ya da eksik seçenek: {}"), {problem}) + L"\n\n";
    text += tr(L"ShadeTubeSetup.exe [/S] [/D=<klasör>] [/DESKTOP] [/NOSTARTMENU] [/AUTOSTART] [/LAUNCH]\n"
               L"ShadeTubeSetup.exe /UNINSTALL [/S] [/REMOVEDATA]\n\n"
               L"/S  sessiz: pencere ve soru yok, sonuç çıkış kodunda\n"
               L"/D=<klasör>  kurulum klasörü (en sonda, tırnaksız da olur)\n"
               L"/DESKTOP  masaüstü kısayolu da\n"
               L"/NOSTARTMENU  Başlat menüsü kısayolu olmasın\n"
               L"/AUTOSTART  Windows ile başlat\n"
               L"/LAUNCH  bitince ShadeTube'u aç\n"
               L"/UNINSTALL  kurulu kopyayı kaldır (/REMOVEDATA: ayarlar ve veriler de)\n\n"
               L"Çıkış kodları: 0 tamam, 1 hata, 87 geçersiz seçenek, 1602 iptal, 1618 başka bir kurulum çalışıyor.");
    MessageBoxW(nullptr, text.c_str(), tr(L"ShadeTube kurulumu"), MB_OK | (problem.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
}

bool rawSilent(std::wstring_view args) {
    for (const wchar_t* s : {L"/S", L"/s", L"-S", L"-s"}) {
        const size_t at = args.find(s);
        if (at != std::wstring_view::npos && (at + 2 == args.size() || args[at + 2] == L' ')) return true;
    }
    return false;
}

// Silent: the same work as the window, with the choices from the command line (an update keeps the shortcuts as they
// are). The exit code is the result; everything else goes to the log.
int silent(const Args& args, std::string_view payloadData, const payload::Info& info) {
    const Existing e = detect();
    Plan plan;
    if (args.uninstall) {
        plan.action = Action::Uninstall;
        plan.removeData = args.removeData;
    } else {
        plan.action = actionFor(e, info.version);
        plan.dir = !args.dir.empty() ? std::filesystem::absolute(std::filesystem::path(args.dir)) : e.installed ? e.dir : defaultFolder();
        plan.startMenu = !args.noStartMenu && (!e.installed || e.startMenu || e.exeMissing);
        plan.desktop = args.desktop || (e.installed && e.desktop);
        if (args.autostart) plan.autostart = true;
    }
    ST_LOG_INFO("setup", "silent {} (installed {}, dir {})", args.uninstall ? "uninstall" : "install", e.installed ? e.version : "-",
                toUtf8(plan.dir.wstring()));
    const Result r = run(plan, payloadData);
    if (!r.ok) return kExitFailed;
    if (args.launch && !args.uninstall && !r.exe.empty())
        installer::launch(r.exe, plan.action == Action::Update ? L"--updated" : L"--installed");
    return kExitOk;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    // SHADETUBE_SETUP_SANDBOX: every location into a scratch folder / test key (tests, screenshots).
    const std::wstring sandbox = installer::applySandbox();
    const std::wstring_view rawArgs = argumentsOf(GetCommandLineW());
    std::wstring argError;
    const auto args = parseArgs(rawArgs, &argError);
    i18n::init(args ? args->lang : std::string());   // the UI language follows Windows (among the app's)
    if (!args) {
        if (!rawSilent(rawArgs)) usage(argError);
        return kExitBadArgs;
    }
    if (args->help) {
        usage({});
        return kExitOk;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    log::init();
    ST_LOG_INFO("setup", "ShadeTube setup{}{} ({})", args->silent ? " /S" : "", sandbox.empty() ? "" : " in a sandbox",
                toUtf8(std::wstring(rawArgs)));

    const std::string_view data = payload::embedded();
    payload::Info info;
    std::wstring payloadError;
    try {
        info = payload::read(data);
        ST_LOG_INFO("setup", "payload ShadeTube {}: {} file(s), {} bytes", info.version, info.files.size(), info.totalSize);
    } catch (const std::exception& e) {
        payloadError = toWide(e.what());
        ST_LOG_ERROR("setup", "payload: {}", e.what());
    }

    int code = kExitOk;
    if (!args->screenshotDir.empty()) {
        gfx::Device::get().init();
        gfx::Fonts::init();
        code = screenshots(*args, data, info);
        gfx::Device::get().shutdown();
    } else {
        // One setup at a time (per sandbox): a second interactive one brings the first to the front.
        const std::wstring mutexName = L"Local\\ShadeTube.Setup" + (sandbox.empty() ? std::wstring() : L"." + sandbox.substr(sandbox.rfind(L'\\') + 1));
        HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (!args->silent)
                if (HWND other = FindWindowW(L"ShadeTube.Setup", nullptr)) {
                    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                    SetForegroundWindow(other);
                }
            code = kExitAlreadyRunning;
        } else if (args->silent) {
            code = payloadError.empty() ? silent(*args, data, info) : kExitFailed;
        } else {
            gfx::Device::get().init();
            gfx::Fonts::init();
            code = runWindow(*args, data, info, payloadError);
            gfx::Device::get().shutdown();
        }
        if (mutex) CloseHandle(mutex);
    }
    ST_LOG_INFO("setup", "exit {}", code);
    log::shutdown();
    CoUninitialize();
    return code;
}
