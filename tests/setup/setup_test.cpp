// Developer test for the setup (src/setup + app/Installer), offline, everything inside a sandbox:
//   payload     pack / read / unpack with every algorithm (blocks, a file across blocks, an empty file, progress),
//               damage (a flipped byte, the seal, truncation, garbage), forged-but-sealed headers (format, names,
//               block count, a file's SHA-256: unpack leaves nothing behind), cancel, and the payload embedded in this
//               build's ShadeTubeSetup.exe (its ShadeTube.exe byte for byte the build's, passing verifyExe)
//   args        the command line (/S, /D= with spaces and quotes, the flags, errors, dev flags) and argumentsOf()
//   engine      actionFor / folderFor / writable / freeSpace; in SHADETUBE_SETUP_SANDBOX: a fresh install (files,
//               the uninstall key's values, both shortcuts, the Run value + settings.json), a repair that changes the
//               options (shortcut removed, settings.json's other values kept), an update with this build's app
//               running from the install folder (asked to quit, exit code 0), a move to another folder, uninstall
//               keeping / removing the user data, `ShadeTube.exe --uninstall --quiet` on a setup install
//   silent      ShadeTubeSetup.exe /S, /D=<folder with spaces> /DESKTOP, /UNINSTALL, bad switches (87), another setup
//               running (1618)
// The sandbox (%TEMP%\ShadeTube-setup-test-<pid>) and its test registry key HKCU\Software\ShadeTubeSandbox\<id> are
// removed at the end. Nothing outside them is written: the test refuses to run when the redirection did not apply.
//   setup_test [payload|args|engine|silent]...   (default: all)
#include "app/Autostart.h"
#include "app/Installer.h"
#include "app/Updater.h"
#include "core/Utf.h"
#include "setup/Payload.h"
#include "setup/SetupArgs.h"
#include "setup/SetupEngine.h"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ins = st::app::installer;
namespace up = st::app::updater;
using namespace st::setup;

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

#define CHECK_THROWS(expr, needle)                                                                        \
    do {                                                                                                  \
        std::string msg_;                                                                                 \
        bool threw_ = false;                                                                              \
        try {                                                                                             \
            expr;                                                                                         \
        } catch (const std::exception& e) {                                                               \
            threw_ = true;                                                                                \
            msg_ = e.what();                                                                              \
        }                                                                                                 \
        const bool ok_ = threw_ && msg_.find(needle) != std::string::npos;                                \
        std::printf("  %s %s -> %s\n", ok_ ? "ok   " : "FAIL ", #expr, threw_ ? msg_.c_str() : "(no exception)"); \
        if (!ok_) {                                                                                       \
            std::printf("        (line %d, wanted \"%s\")\n", __LINE__, needle);                          \
            ++g_failures;                                                                                 \
        }                                                                                                 \
    } while (0)

void section(const char* name) { std::printf("\n== %s\n", name); }

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeAll(const fs::path& p, std::string_view data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool waitFor(const std::function<bool()>& done, int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;) {
        if (done()) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(100);
    }
}

const fs::path& root() {
    static const fs::path r = fs::temp_directory_path() / (L"ShadeTube-setup-test-" + std::to_wstring(GetCurrentProcessId()));
    return r;
}

// Bytes that compress a little (text runs between noise), so blocks differ in size.
std::string mixedBytes(size_t n) {
    std::string s;
    s.reserve(n);
    uint64_t x = 20261127;
    while (s.size() < n) {
        for (int i = 0; i < 700 && s.size() < n; ++i) {
            x = (x * 6364136223846793005ull + 1442695040888963407ull);
            s += static_cast<char>(x >> 56);
        }
        const std::string text = "ShadeTube kurulum testi — müzik çalar " + std::to_string(s.size()) + "\n";
        for (int i = 0; i < 20 && s.size() < n; ++i) s += text.substr(0, std::min(text.size(), n - s.size()));
    }
    return s;
}

std::string reseal(std::string data) {
    data.resize(data.size() - 32);
    const auto seal = payload::sha256(data);
    data.append(reinterpret_cast<const char*>(seal.data()), 32);
    return data;
}

HANDLE start(const fs::path& exe, const std::wstring& args, DWORD flags = CREATE_NO_WINDOW) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

DWORD runToEnd(const fs::path& exe, const std::wstring& args, DWORD timeoutMs = 120000) {
    HANDLE p = start(exe, args);
    if (!p) return 0xFFFFFFFF;
    DWORD code = 0xFFFFFFFE;
    if (WaitForSingleObject(p, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(p, &code);
    else TerminateProcess(p, 99);
    CloseHandle(p);
    return code;
}

std::string embeddedPayload(const fs::path& exe) {
    const HMODULE m = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return {};
    std::string out;
    // rc.exe keeps the quotes of a quoted name (core/Resources does the same lookup).
    if (const HRSRC r = FindResourceW(m, L"\"payload\"", RT_RCDATA))
        if (const HGLOBAL g = LoadResource(m, r)) out.assign(static_cast<const char*>(LockResource(g)), SizeofResource(m, r));
    FreeLibrary(m);
    return out;
}

// SetEnvironmentVariableW (applySandbox) does not reach the CRT's copy that _wgetenv reads.
fs::path envPath(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? fs::path(std::wstring(buf, n)) : fs::path();
}

std::string settingsJson() { return readAll(envPath(L"SHADETUBE_DATA_DIR") / L"settings.json"); }

// ---- payload --------------------------------------------------------------------------------------------------

void testPayload() {
    section("payload: round trip");
    const fs::path src = root() / "payload-src";
    const std::string big = mixedBytes(3'300'000), text = std::string(50'000, 'a') + "son";
    writeAll(src / "a.bin", big);
    writeAll(src / "b.txt", text);
    writeAll(src / "c.empty", "");
    const std::vector<std::pair<std::string, fs::path>> files = {{"a.bin", src / "a.bin"}, {"c.empty", src / "c.empty"}, {"b.txt", src / "b.txt"}};
    for (const auto& [name, algo] : std::vector<std::pair<const char*, uint32_t>>{
             {"LZMS", payload::kLzms}, {"XPRESS_HUFF", payload::kXpressHuff}, {"XPRESS", payload::kXpress}, {"MSZIP", payload::kMszip}}) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string data;
        try {
            data = payload::pack("1.2.3", files, algo, 1u << 20);
        } catch (const std::exception& e) {
            std::printf("  FAIL  pack %s: %s\n", name, e.what());
            ++g_failures;
            continue;
        }
        const auto t1 = std::chrono::steady_clock::now();
        payload::Info info;
        try {
            info = payload::read(data);
        } catch (const std::exception& e) {
            std::printf("  FAIL  read %s: %s\n", name, e.what());
            ++g_failures;
            continue;
        }
        CHECK(info.version == "1.2.3");
        CHECK(info.algorithm == algo);
        CHECK(info.files.size() == 3 && info.files[0].name == "a.bin" && info.files[1].name == "c.empty" && info.files[2].name == "b.txt");
        CHECK(info.files[1].size == 0);
        CHECK(info.totalSize == big.size() + text.size());
        CHECK(info.blocks.size() == (info.totalSize + (1u << 20) - 1) / (1u << 20));
        CHECK(payload::hex(info.files[2].sha256) == payload::hex(payload::sha256(text)));
        const fs::path out = root() / (std::string("payload-out-") + name);
        std::vector<uint64_t> calls;
        std::vector<fs::path> written;
        try {
            written = payload::unpack(data, out, [&](uint64_t done, uint64_t total) {
                calls.push_back(done);
                if (total != info.totalSize) calls.push_back(0);
            });
        } catch (const std::exception& e) {
            std::printf("  FAIL  unpack %s: %s\n", name, e.what());
            ++g_failures;
        }
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(written.size() == 3);
        CHECK(readAll(out / "a.bin") == big);
        CHECK(readAll(out / "b.txt") == text);
        CHECK(present(out / "c.empty") && fs::file_size(out / "c.empty") == 0);
        CHECK(calls.size() == info.blocks.size() && !calls.empty() && calls.back() == info.totalSize);
        CHECK(std::is_sorted(calls.begin(), calls.end()));
        std::printf("        %s: %zu -> %zu bytes, %zu blocks, pack %lld ms, unpack %lld ms\n", name, static_cast<size_t>(info.totalSize),
                    data.size(), info.blocks.size(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()));
    }

    section("payload: damage");
    const std::string good = payload::pack("1.2.3", files, payload::kXpressHuff, 1u << 20);
    std::string bad = good;
    bad[bad.size() / 2] ^= 0x40;   // inside the blocks
    CHECK_THROWS(payload::read(bad), "bozuk");
    bad = good;
    bad[bad.size() - 5] ^= 1;      // the seal itself
    CHECK_THROWS(payload::read(bad), "bozuk");
    CHECK_THROWS(payload::read(good.substr(0, good.size() - 1)), "bozuk");
    CHECK_THROWS(payload::read(good.substr(0, good.size() / 2)), "bozuk");
    CHECK_THROWS(payload::read(std::string("MZ\x90\x00", 4) + std::string(100, 'x')), "bozuk");
    CHECK_THROWS(payload::read(""), "bozuk");

    // Forged headers that carry a valid seal: the structure checks still hold.
    const auto offsetOf = [&](std::string_view needle) { return good.find(needle); };
    bad = good;
    bad[8] = 2;   // format 2
    CHECK_THROWS(payload::read(reseal(bad)), "bozuk");
    bad = good;
    bad[offsetOf("b.txt")] = '\\';   // "\.txt": a name with a folder separator
    CHECK_THROWS(payload::read(reseal(bad)), "bozuk");
    bad = good;
    bad[20] ^= 1;   // the block count
    CHECK_THROWS(payload::read(reseal(bad)), "bozuk");
    bad = good;
    const size_t shaAt = offsetOf("b.txt") + 5 + 8;   // b.txt's SHA-256 after its name and size
    bad[shaAt] ^= 0xFF;
    const std::string forged = reseal(bad);
    CHECK(payload::read(forged).files.size() == 3);   // the header is consistent...
    const fs::path forgedOut = root() / "payload-forged";
    CHECK_THROWS(payload::unpack(forged, forgedOut), "bozuk");   // ...but the file is not what it claims
    CHECK(!present(forgedOut / "a.bin") && !present(forgedOut / "b.txt") && !present(forgedOut / "c.empty"));

    CHECK_THROWS(payload::pack("1.0.0", {{"../x", src / "a.bin"}}), "bad file name");
    CHECK_THROWS(payload::pack("1.0.0", {{"a\\b", src / "a.bin"}}), "bad file name");
    CHECK_THROWS(payload::pack("1.0.0", {}), "files");
    CHECK_THROWS(payload::pack("1.0.0", {{"x.bin", src / "missing.bin"}}), "cannot read");

    section("payload: cancel");
    const fs::path cancelOut = root() / "payload-cancel";
    int blocks = 0;
    CHECK_THROWS(payload::unpack(good, cancelOut, {}, [&] { return ++blocks > 1; }), "iptal");
    CHECK(!present(cancelOut / "a.bin"));

    section("payload: the one in ShadeTubeSetup.exe");
    const std::string embedded = embeddedPayload(ST_SETUP_EXE);
    CHECK(!embedded.empty());
    if (embedded.empty()) return;
    payload::Info info;
    try {
        info = payload::read(embedded);
    } catch (const std::exception& e) {
        std::printf("  FAIL  read: %s\n", e.what());
        ++g_failures;
        return;
    }
    const std::string appVersion = up::normalizeVersion(up::fileProductVersion(ST_APP_EXE));
    std::printf("        %s, %zu files, %llu -> %zu bytes (algorithm %u)\n", info.version.c_str(), info.files.size(),
                static_cast<unsigned long long>(info.totalSize), embedded.size(), info.algorithm);
    CHECK(info.version == appVersion);
    CHECK(info.files.size() == 4 && info.files[0].name == "ShadeTube.exe" && info.files[1].name == "LICENSE.txt" &&
          info.files[2].name == "THIRD_PARTY_NOTICES.md" && info.files[3].name == "OFL.txt");
    const fs::path out = root() / "payload-embedded";
    const auto t0 = std::chrono::steady_clock::now();
    try {
        payload::unpack(embedded, out);
    } catch (const std::exception& e) {
        std::printf("  FAIL  unpack: %s\n", e.what());
        ++g_failures;
    }
    std::printf("        unpacked in %lld ms\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    CHECK(readAll(out / "ShadeTube.exe") == readAll(ST_APP_EXE));
    bool verified = true;
    try {
        up::verifyExe(out / "ShadeTube.exe", info.version);
    } catch (const std::exception&) {
        verified = false;
    }
    CHECK(verified);
    CHECK(up::fileProductName(ST_SETUP_EXE) == "ShadeTube Setup");   // never mistaken for the app by the updater
}

// ---- args -----------------------------------------------------------------------------------------------------

void testArgs() {
    section("command line");
    auto parse = [](std::wstring_view s) { return parseArgs(s); };
    const auto none = parse(L"");
    CHECK(none && !none->silent && none->dir.empty() && !none->uninstall && !none->help);
    for (const wchar_t* s : {L"/S", L"/s", L"-S", L"/SILENT", L"  /S  "}) {
        const auto a = parse(s);
        CHECK(a && a->silent);
    }
    auto a = parse(L"/S /D=C:\\Program Files\\My Apps\\ShadeTube");
    CHECK(a && a->silent && a->dir == L"C:\\Program Files\\My Apps\\ShadeTube");
    a = parse(L"/S /D=\"C:\\x y\\ShadeTube\"");
    CHECK(a && a->dir == L"C:\\x y\\ShadeTube");
    a = parse(L"/S \"/D=C:\\x y\"");
    CHECK(a && a->silent && a->dir == L"C:\\x y");
    a = parse(L"/D=D:\\Apps\\ShadeTube   ");
    CHECK(a && a->dir == L"D:\\Apps\\ShadeTube");
    a = parse(L"/desktop /NoStartMenu /AUTOSTART /launch");
    CHECK(a && a->desktop && a->noStartMenu && a->autostart && a->launch && !a->silent);
    a = parse(L"/UNINSTALL /REMOVEDATA /S");
    CHECK(a && a->uninstall && a->removeData && a->silent);
    a = parse(L"/?");
    CHECK(a && a->help);
    a = parse(L"/HELP");
    CHECK(a && a->help);
    a = parse(L"--screenshot \"C:\\shots dir\" --theme light --scale 1.5 --lang de --page done");
    CHECK(a && a->screenshotDir == L"C:\\shots dir" && a->light && *a->light && a->scale == 1.5f && a->lang == "de" && a->page == "done");
    std::wstring err;
    CHECK(!parseArgs(L"/S /BOGUS", &err) && err == L"/BOGUS");
    CHECK(!parseArgs(L"S", &err) && err == L"S");
    CHECK(!parseArgs(L"/D=", &err));
    CHECK(!parseArgs(L"/REMOVEDATA", &err) && err == L"/REMOVEDATA");
    CHECK(!parseArgs(L"/UNINSTALL /D=C:\\x", &err));
    CHECK(!parseArgs(L"--theme blue", &err) && err == L"--theme");
    CHECK(!parseArgs(L"--scale 9", &err));
    CHECK(!parseArgs(L"--screenshot", &err));
    CHECK(!parseArgs(L"--bogus", &err));
    CHECK(argumentsOf(L"\"C:\\x y\\ShadeTubeSetup.exe\" /S") == L"/S");
    CHECK(argumentsOf(L"setup.exe   /S /D=C:\\a b") == L"/S /D=C:\\a b");
    CHECK(argumentsOf(L"\"a.exe\"").empty());
    CHECK(argumentsOf(L"").empty());
}

// ---- engine ---------------------------------------------------------------------------------------------------

bool insideSandbox(const fs::path& p, const fs::path& sandbox) {
    const std::wstring a = fs::absolute(p).lexically_normal().wstring(), b = fs::absolute(sandbox).lexically_normal().wstring();
    return a.size() > b.size() && CompareStringOrdinal(a.c_str(), static_cast<int>(b.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring g_sandboxKey;

// The sandbox is in place, or nothing below may run.
bool sandboxReady() {
    static int state = -1;
    if (state >= 0) return state == 1;
    const fs::path sandbox = root() / "sandbox";
    SetEnvironmentVariableW(L"SHADETUBE_SETUP_SANDBOX", sandbox.c_str());
    g_sandboxKey = ins::applySandbox();
    const ins::Locations loc = ins::locations();
    const bool ok = g_sandboxKey.rfind(L"Software\\ShadeTubeSandbox\\", 0) == 0 && insideSandbox(loc.dir, sandbox) &&
                    insideSandbox(loc.shortcut, sandbox) && insideSandbox(loc.desktopShortcut, sandbox) &&
                    loc.uninstallKey.rfind(g_sandboxKey + L"\\", 0) == 0 && st::app::autostart::runKey().rfind(g_sandboxKey + L"\\", 0) == 0 &&
                    st::app::autostart::allowed() && insideSandbox(envPath(L"SHADETUBE_DATA_DIR"), sandbox) &&
                    ins::instanceClass() != L"ShadeTube.Window" && ins::instanceMutex() != L"Local\\ShadeTube.SingleInstance";
    std::printf("        sandbox %ls, key HKCU\\%ls\n", sandbox.c_str(), g_sandboxKey.c_str());
    CHECK(ok);
    if (!ok) std::printf("  skip  the sandbox did not apply: refusing to touch real locations\n");
    if (ok) RegDeleteTreeW(HKEY_CURRENT_USER, g_sandboxKey.c_str());   // leftovers of an aborted run
    state = ok ? 1 : 0;
    return ok;
}

void testEngineLogic() {
    section("engine: decisions");
    Existing e;
    CHECK(actionFor(e, "0.12.0") == Action::Install);
    e.installed = true;
    e.version = "0.11.0";
    CHECK(actionFor(e, "0.12.0") == Action::Update);
    e.version = "0.12.0";
    CHECK(actionFor(e, "0.12.0") == Action::Repair);
    e.version = "0.13.0";
    CHECK(actionFor(e, "0.12.0") == Action::Downgrade);
    e.version = "0.12.0";
    e.exeMissing = true;
    CHECK(actionFor(e, "0.13.0") == Action::Repair);
    CHECK(folderFor(L"D:\\Apps") == fs::path(L"D:\\Apps\\ShadeTube"));
    CHECK(folderFor(L"D:\\Apps\\") == fs::path(L"D:\\Apps\\ShadeTube"));
    CHECK(folderFor(L"D:\\Apps\\ShadeTube") == fs::path(L"D:\\Apps\\ShadeTube"));
    CHECK(folderFor(L"d:\\apps\\shadetube") == fs::path(L"d:\\apps\\shadetube"));
    CHECK(folderFor(L"D:\\") == fs::path(L"D:\\ShadeTube"));
    CHECK(writable(root()));
    CHECK(writable(root() / "not" / "there" / "yet"));   // its nearest existing parent decides
    std::wstring missingDrive;
    const DWORD drives = GetLogicalDrives();
    for (wchar_t d = L'Z'; d > L'D'; --d)
        if (!(drives & (1u << (d - L'A')))) {
            missingDrive = std::wstring(1, d) + L":\\Nope\\ShadeTube";
            break;
        }
    if (!missingDrive.empty()) CHECK(!writable(missingDrive) && freeSpace(missingDrive) == -1);
    CHECK(freeSpace(root()) > 0);
}

void testEngine() {
    testEngineLogic();
    if (!sandboxReady()) return;
    const std::string payloadData = embeddedPayload(ST_SETUP_EXE);
    const payload::Info info = payload::read(payloadData);
    const ins::Locations loc = ins::locations();
    const fs::path dataDir = envPath(L"SHADETUBE_DATA_DIR");

    section("engine: fresh install");
    Existing e = detect();
    CHECK(!e.installed && e.running.empty());
    CHECK(actionFor(e, info.version) == Action::Install);
    writeAll(dataDir / "settings.json", R"({"theme": "light", "volume": 0.5, "futureKey": {"x": 1}})");
    Plan p;
    p.action = Action::Install;
    p.dir = defaultFolder();
    p.startMenu = true;
    p.desktop = true;
    p.autostart = true;
    std::vector<float> seen;
    std::vector<Step> steps;
    const auto t0 = std::chrono::steady_clock::now();
    Result r = run(p, payloadData, [&](Step s, float f) {
        seen.push_back(f);
        steps.push_back(s);
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("        install took %lld ms (%zu progress calls)%s%ls\n", static_cast<long long>(ms), seen.size(), r.ok ? "" : ": ", r.error.c_str());
    CHECK(r.ok);
    CHECK(!seen.empty() && std::is_sorted(seen.begin(), seen.end()) && seen.back() == 1.f && steps.back() == Step::Done);
    CHECK(r.exe == loc.dir / L"ShadeTube.exe");
    CHECK(readAll(r.exe) == readAll(ST_APP_EXE));
    for (const wchar_t* f : {L"LICENSE.txt", L"THIRD_PARTY_NOTICES.md", L"OFL.txt"}) CHECK(present(loc.dir / f));
    CHECK(!present(loc.dir / L"ShadeTube.exe.tmp"));
    const ins::Registration reg = ins::readRegistration();
    CHECK(reg.exists && reg.displayName == L"ShadeTube" && reg.displayVersion == st::toWide(info.version));
    CHECK(reg.installLocation == loc.dir.wstring());
    CHECK(reg.uninstallString == L"\"" + r.exe.wstring() + L"\" --uninstall");
    CHECK(reg.quietUninstallString == L"\"" + r.exe.wstring() + L"\" --uninstall --quiet");
    CHECK(ins::shortcutPointsAt(loc.shortcut, r.exe));
    CHECK(ins::shortcutPointsAt(loc.desktopShortcut, r.exe));
    CHECK(st::app::autostart::read() == st::app::autostart::commandFor(r.exe));
    CHECK(ins::startWithWindowsSetting());
    CHECK(settingsJson().find("futureKey") != std::string::npos && settingsJson().find("light") != std::string::npos);
    e = detect();
    CHECK(e.installed && !e.exeMissing && e.version == info.version && e.startMenu && e.desktop && e.autostart);
    CHECK(actionFor(e, info.version) == Action::Repair);

    section("engine: repair with other options");
    p.action = Action::Repair;
    p.desktop = false;
    p.autostart = false;
    r = run(p, payloadData);
    CHECK(r.ok);
    CHECK(!present(loc.desktopShortcut));   // ours, no longer wanted
    CHECK(present(loc.shortcut));
    CHECK(!st::app::autostart::read().has_value());
    CHECK(!ins::startWithWindowsSetting());
    CHECK(settingsJson().find("futureKey") != std::string::npos);   // a newer version's values stay
    // A desktop shortcut of the user's own (to something else) is never touched.
    writeAll(loc.desktopShortcut, "not a shortcut of ours");
    r = run(p, payloadData);
    CHECK(r.ok && present(loc.desktopShortcut));
    fs::remove(loc.desktopShortcut);

    section("engine: update while ShadeTube runs from the install folder");
    // This build's app, started from the install folder (it inherits the sandbox: profile, window class, mutex).
    HANDLE app = start(r.exe, L"--preview --route settings", 0);
    CHECK(app != nullptr);
    CHECK(waitFor([] { return !ins::runningInstances().empty(); }, 30000));
    e = detect();
    CHECK(e.running.size() == 1);
    r = run(p, payloadData);
    CHECK(r.ok && !r.stillRunning);
    DWORD appExit = 99;
    CHECK(app && WaitForSingleObject(app, 5000) == WAIT_OBJECT_0);   // asked to quit (WM_QUIT) before the files moved
    if (app) {
        GetExitCodeProcess(app, &appExit);
        if (appExit == STILL_ACTIVE) TerminateProcess(app, 1);
        CloseHandle(app);
    }
    std::printf("        the app's exit code %lu (0 = graceful)\n", appExit);
    CHECK(appExit == 0);
    CHECK(readAll(r.exe) == readAll(ST_APP_EXE));

    section("engine: moved to another folder");
    const fs::path other = root() / "sandbox" / "Other Apps" / "ShadeTube";
    writeAll(loc.dir / "notlar.txt", "kullanıcının dosyası");   // never deleted
    p.action = Action::Install;
    p.dir = other;
    r = run(p, payloadData);
    CHECK(r.ok && r.exe == other / L"ShadeTube.exe");
    CHECK(ins::readRegistration().installLocation == other.wstring());
    CHECK(!present(loc.dir / L"ShadeTube.exe") && !present(loc.dir / L"LICENSE.txt"));
    CHECK(present(loc.dir / "notlar.txt"));
    CHECK(ins::shortcutPointsAt(loc.shortcut, r.exe));
    fs::remove_all(loc.dir);

    section("engine: uninstall, keeping the data");
    p.action = Action::Uninstall;
    p.removeData = false;
    r = run(p, payloadData);
    CHECK(r.ok);
    CHECK(!ins::readRegistration().exists && !present(loc.shortcut) && !present(loc.desktopShortcut));
    CHECK(waitFor([&] { return !present(other); }, 20000));
    CHECK(present(dataDir / "settings.json"));
    e = detect();
    CHECK(!e.installed);
    r = run(p, payloadData);   // nothing installed: still fine
    CHECK(r.ok);

    section("engine: uninstall with the data");
    p.action = Action::Install;
    p.dir = defaultFolder();
    p.autostart = true;
    CHECK(run(p, payloadData).ok);
    CHECK(st::app::autostart::read().has_value());
    p.action = Action::Uninstall;
    p.removeData = true;
    r = run(p, payloadData);
    CHECK(r.ok && !r.dataKept);
    CHECK(!st::app::autostart::read().has_value());
    CHECK(waitFor([&] { return !present(dataDir); }, 30000));
    CHECK(waitFor([&] { return !present(loc.dir); }, 20000));
    Sleep(1500);   // the deferred-delete helper's loop has ended before the profile comes back

    section("ShadeTube.exe --uninstall --quiet on a setup install");
    fs::create_directories(dataDir);
    p.action = Action::Install;
    p.desktop = true;
    p.autostart.reset();
    CHECK(run(p, payloadData).ok);
    CHECK(runToEnd(loc.dir / L"ShadeTube.exe", L"--uninstall --quiet") == 0);
    CHECK(!ins::readRegistration().exists && !present(loc.shortcut) && !present(loc.desktopShortcut));
    CHECK(waitFor([&] { return !present(loc.dir); }, 30000));
}

// ---- silent -----------------------------------------------------------------------------------------------------

void testSilent() {
    if (!sandboxReady()) return;
    const ins::Locations loc = ins::locations();
    section("ShadeTubeSetup.exe /S");
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(runToEnd(ST_SETUP_EXE, L"/S") == kExitOk);
    std::printf("        /S took %lld ms\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    CHECK(ins::readRegistration().exists && present(loc.exe) && present(loc.shortcut) && !present(loc.desktopShortcut));
    const fs::path spaced = root() / "sandbox" / "My Apps" / "Shade Tube";
    CHECK(runToEnd(ST_SETUP_EXE, L"/S /DESKTOP /D=" + spaced.wstring()) == kExitOk);
    CHECK(ins::readRegistration().installLocation == spaced.wstring());
    CHECK(present(spaced / L"ShadeTube.exe") && !present(loc.exe));
    CHECK(ins::shortcutPointsAt(loc.desktopShortcut, spaced / L"ShadeTube.exe"));
    CHECK(runToEnd(ST_SETUP_EXE, L"/S /UNINSTALL") == kExitOk);
    CHECK(!ins::readRegistration().exists && !present(loc.shortcut) && !present(loc.desktopShortcut));
    CHECK(waitFor([&] { return !present(spaced); }, 30000));
    CHECK(runToEnd(ST_SETUP_EXE, L"/S /UNINSTALL") == kExitOk);   // nothing to remove
    CHECK(runToEnd(ST_SETUP_EXE, L"/S /BOGUS") == kExitBadArgs);
    CHECK(runToEnd(ST_SETUP_EXE, L"/S /REMOVEDATA") == kExitBadArgs);
    // Another setup of this sandbox is running: 1618, nothing done.
    const std::wstring mutexName = L"Local\\ShadeTube.Setup." + g_sandboxKey.substr(g_sandboxKey.rfind(L'\\') + 1);
    const HANDLE m = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    CHECK(runToEnd(ST_SETUP_EXE, L"/S") == kExitAlreadyRunning);
    if (m) CloseHandle(m);
    CHECK(!ins::readRegistration().exists);
}

void cleanup() {
    if (!g_sandboxKey.empty()) {
        RegDeleteTreeW(HKEY_CURRENT_USER, g_sandboxKey.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\ShadeTubeSandbox");   // only when no other sandbox has a key
        HKEY k = nullptr;
        const bool gone = RegOpenKeyExW(HKEY_CURRENT_USER, g_sandboxKey.c_str(), 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND;
        if (k) RegCloseKey(k);
        CHECK(gone);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // every line out at once (child processes, long waits)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    fs::remove_all(root(), ec);
    fs::create_directories(root());
    auto wanted = [&](const wchar_t* name) {
        if (argc < 2) return true;
        for (int i = 1; i < argc; ++i)
            if (std::wstring_view(argv[i]) == name) return true;
        return false;
    };
    if (wanted(L"payload")) testPayload();
    if (wanted(L"args")) testArgs();
    if (wanted(L"engine")) testEngine();
    if (wanted(L"silent")) testSilent();
    cleanup();
    // Folders the deferred-delete helper may still be working on: give it a moment, then remove what is left.
    Sleep(500);
    fs::remove_all(root(), ec);
    CoUninitialize();
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
