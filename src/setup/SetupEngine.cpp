// The setup's work: detect, install / update / repair / downgrade, uninstall. See SetupEngine.h.
#include "setup/SetupEngine.h"

#include "app/Autostart.h"
#include "app/Updater.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "setup/Payload.h"

#include <windows.h>

#include <algorithm>
#include <format>

namespace st::setup {

namespace fs = std::filesystem;
namespace installer = app::installer;
namespace updater = app::updater;

namespace {

bool iequals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) || iequals(a.lexically_normal().wstring(), b.lexically_normal().wstring());
}

fs::path nearestExisting(fs::path p) {
    std::error_code ec;
    p = fs::absolute(p, ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        if (p == p.parent_path()) return {};
        p = p.parent_path();
    }
    return p;
}

// A unique staging folder under %TEMP% (removed again by run()).
fs::path stagingFolder() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / std::format(L"ShadeTubeSetup-{}-{}", GetCurrentProcessId(), GetTickCount64());
}

// The single-instance mutex of an exiting app goes a moment after its windows: wait for it (bounded).
void waitForMutex(DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (installer::instanceMutexHeld() && GetTickCount64() < until) Sleep(100);
}

// An install moved to another folder: the old folder's own files go (anything else in it stays), the folder too when
// that leaves it empty. Files still in use are deleted once they are free (deleteAfterExit's retry loop).
void removeOldFolder(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> later;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (installer::isOwnFile(e.path()) && !DeleteFileW(e.path().c_str())) later.push_back(e.path());
    if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
    else if (!later.empty()) installer::deleteAfterExit(later);
    ST_LOG_INFO("setup", "previous install folder {} cleared", toUtf8(dir.wstring()));
}

} // namespace

fs::path defaultFolder() { return installer::locations().dir; }

fs::path folderFor(const fs::path& picked) {
    std::error_code ec;
    fs::path p = fs::absolute(picked, ec).lexically_normal();
    while (p.has_relative_path() && p.filename().empty()) p = p.parent_path();   // "D:\Apps\" -> "D:\Apps"
    if (!iequals(p.filename().wstring(), L"ShadeTube")) p /= L"ShadeTube";
    return p;
}

bool writable(const fs::path& dir) {
    const fs::path base = nearestExisting(dir);
    if (base.empty()) return false;
    const fs::path probe = base / std::format(L".shadetube-setup-{}.tmp", GetCurrentProcessId());
    const HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

int64_t freeSpace(const fs::path& dir) {
    const fs::path base = nearestExisting(dir);
    ULARGE_INTEGER avail{};
    if (base.empty() || !GetDiskFreeSpaceExW(base.c_str(), &avail, nullptr, nullptr)) return -1;
    return static_cast<int64_t>(avail.QuadPart);
}

Existing detect() {
    Existing e;
    const installer::Locations loc = installer::locations();
    const installer::Registration reg = installer::readRegistration();
    e.dir = loc.dir;
    if (reg.exists && !reg.installLocation.empty()) {
        e.installed = true;
        e.dir = reg.installLocation;
    }
    e.exe = e.dir / L"ShadeTube.exe";
    std::error_code ec;
    if (e.installed) {
        e.exeMissing = !fs::is_regular_file(e.exe, ec);
        if (!e.exeMissing) e.version = updater::normalizeVersion(updater::fileProductVersion(e.exe));
        e.startMenu = installer::shortcutPointsAt(loc.shortcut, e.exe);
        e.desktop = installer::shortcutPointsAt(loc.desktopShortcut, e.exe);
    }
    // "Windows ile başlat" belongs to every copy (settings.json is shared): the app's setting and its Run value.
    const auto run = app::autostart::read();
    e.autostart = installer::startWithWindowsSetting() && run.has_value();
    e.running = installer::runningInstances();
    return e;
}

Action actionFor(const Existing& e, std::string_view setupVersion) {
    if (!e.installed) return Action::Install;
    if (e.exeMissing || e.version.empty()) return Action::Repair;
    const int c = updater::compareVersions(e.version, setupVersion);
    return c < 0 ? Action::Update : c == 0 ? Action::Repair : Action::Downgrade;
}

Result run(const Plan& plan, std::string_view payloadData, const ProgressFn& progress) {
    Result r;
    auto report = [&](Step s, float f) {
        if (progress) progress(s, std::clamp(f, 0.f, 1.f));
    };
    const fs::path staging = stagingFolder();
    try {
        if (plan.action == Action::Uninstall) {
            const Existing now = detect();
            if (!now.installed) {
                // Nothing registered: still clear what an aborted run may have left (shortcuts, the key).
                installer::unregister();
                r.ok = true;
                report(Step::Done, 1);
                return r;
            }
            // The copy running from the install folder keeps it in use and has to go; copies running from anywhere
            // else only matter when the shared data folder is deleted too.
            report(Step::Close, 0);
            std::vector<installer::Instance> inDir, others;
            for (auto& i : now.running) (!i.image.empty() && samePath(i.image.parent_path(), now.dir) ? inDir : others).push_back(std::move(i));
            if (!inDir.empty()) {
                if (!installer::closeInstances(inDir)) {
                    r.stillRunning = true;
                    throw std::runtime_error(toUtf8(tr(L"ShadeTube kapatılamadı. Uygulamayı kapatıp yeniden dene.")));
                }
                r.steps.push_back(L"closed");
            }
            bool removeData = plan.removeData;
            if (removeData && ((!others.empty() && !installer::closeInstances(others)) || (waitForMutex(3000), installer::instanceMutexHeld()))) {
                ST_LOG_WARN("setup", "another ShadeTube is still running: user data kept");
                removeData = false;
                r.dataKept = true;
            }
            report(Step::Remove, 0.4f);
            installer::uninstall(removeData);
            r.steps.push_back(removeData ? L"uninstalled+data" : L"uninstalled");
            r.ok = true;
            report(Step::Done, 1);
            ST_LOG_INFO("setup", "uninstalled ({})", removeData ? "user data removed" : "user data kept");
            return r;
        }

        // Install / update / repair / downgrade.
        report(Step::Verify, 0);
        if (payloadData.empty()) throw std::runtime_error(toUtf8(tr(L"Kurulum paketi bozuk. ShadeTube'u yeniden indir.")));
        const payload::Info info = payload::read(payloadData);   // the seal: nothing is trusted before it matches
        report(Step::Verify, 0.08f);

        const Existing before = detect();
        // Every running ShadeTube quits first (each saves its settings, library and queue on the way out): the files
        // are replaced, and the settings it would write at its exit are shared by every copy.
        if (!before.running.empty()) {
            report(Step::Close, 0.08f);
            if (!installer::closeInstances(before.running)) {
                // Not fatal: the installed exe is renamed away (install() -> swapExe) and the new one is in place for
                // the next start; the parked file is removed then.
                r.stillRunning = true;
                ST_LOG_WARN("setup", "a ShadeTube did not quit; its exe is replaced by renaming");
            } else {
                waitForMutex(5000);
                r.steps.push_back(L"closed");
            }
        }
        report(Step::Unpack, 0.15f);
        const auto files = payload::unpack(payloadData, staging, [&](uint64_t done, uint64_t total) {
            report(Step::Unpack, 0.15f + 0.6f * static_cast<float>(static_cast<double>(done) / static_cast<double>(std::max<uint64_t>(total, 1))));
        });
        const fs::path stagedExe = staging / L"ShadeTube.exe";
        // The app inside: a 64-bit Windows GUI exe named ShadeTube, of the version the setup was built for.
        updater::verifyExe(stagedExe, info.version);
        report(Step::Copy, 0.78f);

        installer::InstallOptions o;
        o.dir = plan.dir.empty() ? defaultFolder() : plan.dir;
        o.startMenuShortcut = plan.startMenu;
        o.desktopShortcut = plan.desktop;
        for (const auto& f : files)
            if (!iequals(f.filename().wstring(), L"ShadeTube.exe")) o.extraFiles.push_back(f);
        installer::install(stagedExe, o);
        r.exe = o.dir / L"ShadeTube.exe";
        r.steps.push_back(L"files");
        report(Step::Register, 0.9f);
        if (before.installed && !samePath(before.dir, o.dir)) {
            // Moved: the old folder's files go, and a shortcut still starting the old exe was not asked for again.
            removeOldFolder(before.dir);
            const installer::Locations loc = installer::locations();
            for (const auto& lnk : {loc.shortcut, loc.desktopShortcut})
                if (installer::shortcutPointsAt(lnk, before.exe)) DeleteFileW(lnk.c_str());
        }
        if (plan.autostart && *plan.autostart != before.autostart) installer::setStartWithWindows(*plan.autostart, r.exe);
        else if (before.autostart) app::autostart::sync(true, r.exe);   // keep it pointing at this exe
        r.steps.push_back(L"registered");
        r.ok = true;
        ST_LOG_INFO("setup", "{} {} into {}", plan.action == Action::Install ? "installed" : plan.action == Action::Update ? "updated to" : "reinstalled",
                    info.version, toUtf8(o.dir.wstring()));
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = toWide(e.what());
        ST_LOG_ERROR("setup", "failed: {}", e.what());
    }
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (r.ok) report(Step::Done, 1);
    return r;
}

} // namespace st::setup
