#pragma once
// What ShadeTubeSetup.exe does, without its window (the silent mode and setup_test use it directly): look at this PC,
// then install / update / repair / downgrade from the payload or uninstall, reporting progress. Everything installs
// through app/Installer (st_install), the same code the app's own uninstall and registration refresh use.
// run() blocks (seconds at most): the window calls it on a worker thread.
#include "app/Installer.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::setup {

// This PC as the setup finds it.
struct Existing {
    bool installed = false;          // the uninstall key names a folder
    bool exeMissing = false;         // ... but its ShadeTube.exe is gone (a repair puts it back)
    std::filesystem::path dir;       // the installed folder, else the default one
    std::filesystem::path exe;       // dir\ShadeTube.exe
    std::string version;             // the installed exe's ProductVersion ("" when unknown)
    bool startMenu = false, desktop = false, autostart = false;   // as they are now
    std::vector<app::installer::Instance> running;   // ShadeTubes running now (any folder)
};
Existing detect();

enum class Action { Install, Update, Repair, Downgrade, Uninstall };
// The install-like action for this PC and the setup's version: nothing installed = Install, older = Update, the same
// (or the exe missing) = Repair, newer = Downgrade.
Action actionFor(const Existing& e, std::string_view setupVersion);

struct Plan {
    Action action = Action::Install;
    std::filesystem::path dir;          // install folder (the installed one for everything but a fresh install)
    bool startMenu = true, desktop = false;
    std::optional<bool> autostart;      // nullopt: leave "Windows ile başlat" as it is
    bool removeData = false;            // Uninstall: also the settings, session, library, caches
};

enum class Step { Verify, Close, Unpack, Copy, Register, Remove, Done };
// `overall`: 0..1 for the whole job (the steps have fixed shares, Unpack moves with every block).
using ProgressFn = std::function<void(Step step, float overall)>;

struct Result {
    bool ok = false;
    std::wstring error;                // UI language
    std::filesystem::path exe;         // the installed exe
    bool stillRunning = false;         // a ShadeTube did not quit (install: its exe was renamed away and is replaced
                                       // at its next start; uninstall: never, that is an error)
    bool dataKept = false;             // Uninstall + removeData: another ShadeTube kept running, the data stayed
    std::vector<std::wstring> steps;   // what was done, for the log / the done page
};
Result run(const Plan& plan, std::string_view payload, const ProgressFn& progress = {});

// The default install folder (app/Installer's, or the sandbox's).
std::filesystem::path defaultFolder();
// A folder the user picked: "\ShadeTube" is appended unless that is its name already (never straight into
// "D:\Programs" or a drive root).
std::filesystem::path folderFor(const std::filesystem::path& picked);
// The folder can be written without admin rights: a temp file is created and deleted in it (or in its nearest
// existing parent, when it does not exist yet).
bool writable(const std::filesystem::path& dir);
// Free bytes on the volume of `dir` (nearest existing parent); -1 when unknown.
int64_t freeSpace(const std::filesystem::path& dir);

} // namespace st::setup
