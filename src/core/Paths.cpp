#include "core/Paths.h"

#include <windows.h>
#include <shlobj.h>

namespace st::paths {

namespace fs = std::filesystem;

static fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
}

static fs::path ensure(fs::path p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// SHADETUBE_DATA_DIR (dev / tests): use another profile folder instead of %LOCALAPPDATA%\ShadeTube, so a test run
// never touches the user's settings, library, session or caches.
const fs::path& appData() {
    static const fs::path dir = [] {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) return ensure(fs::path(buf));
        return ensure(knownFolder(FOLDERID_LocalAppData) / L"ShadeTube");
    }();
    return dir;
}

fs::path logDir() { return ensure(appData() / L"logs"); }
fs::path cacheDir() { return ensure(appData() / L"cache"); }
fs::path imageCacheDir() { return ensure(cacheDir() / L"images"); }
fs::path downloadsDir() { return ensure(knownFolder(FOLDERID_Music) / L"ShadeTube"); }
fs::path settingsFile() { return appData() / L"settings.json"; }

} // namespace st::paths
