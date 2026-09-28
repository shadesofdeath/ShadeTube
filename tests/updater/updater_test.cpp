// Developer test for app/Updater + app/Installer (no UI):
//   - versions: parse, semver precedence, normalize
//   - release JSON (fixtures/release_latest.json): fields, asset digest, "sha256:" lines of the notes, asset choice
//   - ZIP reader: fixtures/sample.zip (Compress-Archive: deflate, '\' folders, stored blocks), a hand-made stored zip
//     with a comment, corrupt / truncated input, and the real package of tools/package.ps1 (dist\ShadeTube-<ver>-
//     win64.zip, when present) whose exe must match the staged one byte for byte and pass verifyExe
//   - swapExe on copies in a temp dir (also while the target runs), rollback, cleanupAfterUpdate
//   - installer against overridden locations (SHADETUBE_INSTALL_DIR / _SHORTCUT_DIR / _UNINSTALL_KEY =
//     HKCU\Software\ShadeTubeTest\...): shortcut target, registry values, reinstall over a running exe, uninstall
//     (immediate, deferred helper, foreign files kept, user data removed), and `--uninstall --quiet` closing an
//     app instance that runs from the install folder
//   - deleteAfterExit, launchAfterExit (the relaunch starts only after main() returned)
//   updater_test                 offline tests (%TEMP%\ShadeTube-updater-test-<pid>, removed at the end)
//   updater_test --e2e <base>    check / download / install against the mock server (tests\updater\run_e2e.ps1)
// Child modes used by the tests themselves: --sleep <ms>, --relaunch <marker>, --write-marker <marker> <mutex>,
// --fake-instance <class> quits|stubborn, --uninstall [--quiet] (installer::runUninstallCommand, as main.cpp forwards it)
#include "app/Installer.h"
#include "app/Updater.h"
#include "core/Utf.h"

#include <windows.h>
#include <commctrl.h>   // TDM_CLICK_* (drives the --uninstall TaskDialog)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace up = st::app::updater;
namespace ins = st::app::installer;

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

#define CHECK_NOTHROW(expr)                                                               \
    do {                                                                                  \
        try {                                                                             \
            expr;                                                                         \
            std::printf("  ok    %s\n", #expr);                                           \
        } catch (const std::exception& e) {                                               \
            std::printf("  FAIL  %s threw: %s  (line %d)\n", #expr, e.what(), __LINE__);  \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

// Expects an exception whose message contains `needle`.
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

bool sameContent(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && readAll(a) == readAll(b);
}

bool waitFor(const std::function<bool()>& done, int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;) {
        if (done()) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(100);
    }
}

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Starts `exe args` (no window); returns the process handle.
HANDLE start(const fs::path& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

void finish(HANDLE& process) {
    if (!process) return;
    WaitForSingleObject(process, 30000);
    CloseHandle(process);
    process = nullptr;
}

const fs::path& tempRoot() {
    static const fs::path root = fs::temp_directory_path() / (L"ShadeTube-updater-test-" + std::to_wstring(GetCurrentProcessId()));
    return root;
}

// Bytes of data\random.bin in fixtures/sample.zip (see make_fixture.ps1).
std::string lcgBytes(size_t n) {
    std::string s(n, '\0');
    uint64_t x = 20260928;
    for (size_t i = 0; i < n; ++i) {
        x = (x * 1103515245 + 12345) % 4294967296ull;
        s[i] = static_cast<char>((x >> 16) & 0xff);
    }
    return s;
}

std::string helloText() {
    std::string s;
    for (int i = 0; i < 500; ++i) s += "ShadeTube güncelleme testi " + std::to_string(i) + "\n";
    return s;
}

// Minimal ZIP writer (stored entries) for the reader tests.
std::string storedZip(const std::vector<std::pair<std::string, std::string>>& files, std::string_view comment = {}) {
    auto put16 = [](std::string& s, size_t v) {
        s += static_cast<char>(v & 0xff);
        s += static_cast<char>((v >> 8) & 0xff);
    };
    auto put32 = [&](std::string& s, size_t v) {
        put16(s, v & 0xffff);
        put16(s, (v >> 16) & 0xffff);
    };
    std::string out, cd;
    for (const auto& [name, data] : files) {
        const uint32_t crc = up::crc32(data.data(), data.size());
        const size_t offset = out.size();
        put32(out, 0x04034b50);
        for (int v : {20, 0, 0, 0, 0}) put16(out, static_cast<size_t>(v));   // version, flags, method (stored), time, date
        put32(out, crc);
        put32(out, data.size());
        put32(out, data.size());
        put16(out, name.size());
        put16(out, 0);
        out += name;
        out += data;
        put32(cd, 0x02014b50);
        for (int v : {20, 20, 0, 0, 0, 0}) put16(cd, static_cast<size_t>(v));   // made by, needed, flags, method, time, date
        put32(cd, crc);
        put32(cd, data.size());
        put32(cd, data.size());
        put16(cd, name.size());
        for (int i = 0; i < 4; ++i) put16(cd, 0);   // extra, comment, disk, internal attributes
        put32(cd, 0);                               // external attributes
        put32(cd, offset);
        cd += name;
    }
    const size_t cdOffset = out.size();
    out += cd;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, files.size());
    put16(out, files.size());
    put32(out, cd.size());
    put32(out, cdOffset);
    put16(out, comment.size());
    out += comment;
    return out;
}

// dist\ShadeTube-<ver>-win64.zip with the highest version (tools/package.ps1 output), if any.
fs::path distZip(std::string* version) {
    fs::path best;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(ST_SOURCE_DIR) / "dist", ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("ShadeTube-", 0) != 0 || n.size() < 20 || n.substr(n.size() - 10) != "-win64.zip") continue;
        const std::string v = n.substr(10, n.size() - 20);
        if (best.empty() || up::compareVersions(v, *version) > 0) {
            best = e.path();
            *version = v;
        }
    }
    return best;
}

// ---- Tests -------------------------------------------------------------------------------------------------

void testVersions() {
    section("versions");
    const auto v = up::parseVersion("v0.4.0");
    CHECK(v && v->major == 0 && v->minor == 4 && v->patch == 0 && v->pre.empty());
    const auto p = up::parseVersion(" 1.2.3-beta.1+build.7 ");
    CHECK(p && p->major == 1 && p->minor == 2 && p->patch == 3 && p->pre == "beta.1");
    CHECK(up::parseVersion("0.4") && up::parseVersion("0.4")->patch == 0);
    for (const char* bad : {"", "v", "abc", "1..2", "1.2.", "1.2.3.4", "1.2.3-", "-1.2", "1.x"}) {
        const bool rejected = !up::parseVersion(bad);
        std::printf("  %s parseVersion(\"%s\") rejected\n", rejected ? "ok   " : "FAIL ", bad);
        if (!rejected) ++g_failures;
    }
    CHECK(up::compareVersions("0.4.0", "0.3.0") > 0);
    CHECK(up::compareVersions("v0.3.0", "0.3.0") == 0);
    CHECK(up::compareVersions("0.3", "0.3.0") == 0);
    CHECK(up::compareVersions("v0.10.0", "0.9.9") > 0);
    CHECK(up::compareVersions("1.0.0", "0.99.99") > 0);
    CHECK(up::compareVersions("0.3.1", "0.3.0") > 0);
    CHECK(up::compareVersions("1.0.0+abc", "1.0.0") == 0);
    // semver.org §11 example chain.
    const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    bool ordered = true;
    for (size_t i = 0; i + 1 < std::size(chain); ++i)
        if (up::compareVersions(chain[i], chain[i + 1]) >= 0 || up::compareVersions(chain[i + 1], chain[i]) <= 0) ordered = false;
    CHECK(ordered);
    CHECK(up::compareVersions("garbage", "0.0.1") < 0);
    CHECK(up::normalizeVersion("v0.4") == "0.4.0");
    CHECK(up::normalizeVersion("V1.2.3-rc.1+x") == "1.2.3-rc.1");
    CHECK(up::normalizeVersion("vnext") == "next");
    std::printf("        currentVersion() = %s (this test exe has no VERSIONINFO)\n", up::currentVersion().c_str());
}

void testRelease() {
    section("release JSON");
    const std::string json = readAll(fs::path(ST_UPDATER_FIXTURES) / "release_latest.json");
    CHECK(!json.empty());
    up::Release r;
    CHECK_NOTHROW(r = up::parseRelease(json));
    CHECK(r.tag == "v0.4.0");
    CHECK(r.version == "0.4.0");
    CHECK(r.name == "ShadeTube 0.4.0");
    CHECK(r.htmlUrl == "https://github.com/shadesofdeath/ShadeTube/releases/tag/v0.4.0");
    CHECK(r.publishedAt == "2026-10-05T09:20:01Z");
    CHECK(r.assets.size() == 3);   // the asset without a download URL is dropped
    const up::Asset* a = up::pickAsset(r);
    CHECK(a && a->name == "ShadeTube-0.4.0-win64.zip");
    CHECK(a && a->size == 2031874);
    CHECK(a && a->sha256 == "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
    CHECK(a && a->url == "https://github.com/shadesofdeath/ShadeTube/releases/download/v0.4.0/ShadeTube-0.4.0-win64.zip");
    CHECK(r.notesSha256.size() == 1 && r.notesSha256[0] == "3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b");
    CHECK(r.body.find("Yenilikler") != std::string::npos);

    // Asset choice without the exact name: any ShadeTube win64 zip, else an exe; never arm64 or checksum files.
    up::Release other;
    other.version = "0.5.0";
    other.assets = {{"ShadeTube-arm64.zip", "u1", 1, {}}, {"ShadeTube.exe", "u2", 1, {}}, {"checksums.txt", "u3", 1, {}}};
    CHECK(up::pickAsset(other) && up::pickAsset(other)->name == "ShadeTube.exe");
    other.assets.push_back({"ShadeTube-portable.zip", "u4", 1, {}});
    other.assets.push_back({"ShadeTube-win64-portable.zip", "u5", 1, {}});
    CHECK(up::pickAsset(other) && up::pickAsset(other)->name == "ShadeTube-win64-portable.zip");
    other.assets = {{"notes.txt", "u", 1, {}}};
    CHECK(up::pickAsset(other) == nullptr);

    CHECK(up::findSha256("SHA256: " + std::string(64, 'A') + "\nfoo " + std::string(64, 'b')) ==
          std::vector<std::string>{std::string(64, 'a')});
    CHECK(up::findSha256("sha256 (ShadeTube-0.4.0-win64.zip) = " + std::string(64, 'c') + " " + std::string(65, 'd')) ==
          std::vector<std::string>{std::string(64, 'c')});
    CHECK_THROWS(up::parseRelease("{not json"), "parse");
    CHECK_THROWS(up::parseRelease(R"({"name":"x"})"), "tag_name");
    CHECK_THROWS(up::parseRelease("[1,2]"), "tag_name");
    CHECK(up::updateUrl() == "https://api.github.com/repos/shadesofdeath/ShadeTube/releases/latest");
}

void testZip(const fs::path& root, const fs::path& distZipPath, const std::string& distVersion, fs::path* appExe) {
    section("zip reader");
    CHECK(up::crc32("123456789", 9) == 0xCBF43926u);
    // Raw deflate: one stored block "abc"; an invalid block type; a truncated stream.
    CHECK(up::inflate(std::string("\x01\x03\x00\xfc\xff" "abc", 8), 3) == "abc");
    CHECK_THROWS(up::inflate(std::string("\x07", 1), 3), "deflate");
    CHECK_THROWS(up::inflate(std::string("\x01\x03\x00", 3), 3), "deflate");

    const fs::path sample = fs::path(ST_UPDATER_FIXTURES) / "sample.zip";
    const fs::path out = root / "zip";
    fs::create_directories(out);
    CHECK_NOTHROW(up::extractZipEntry(sample, "hello.txt", out / "hello.txt"));
    CHECK(readAll(out / "hello.txt") == helloText());   // dynamic Huffman
    CHECK_NOTHROW(up::extractZipEntry(sample, "random.bin", out / "random.bin"));
    CHECK(readAll(out / "random.bin") == lcgBytes(65536));   // stored blocks inside deflate, in "data\" ('\' separator)
    CHECK_NOTHROW(up::extractZipEntry(sample, "SMALL.TXT", out / "small.txt"));
    CHECK(readAll(out / "small.txt") == "abc");   // fixed Huffman, case-insensitive name
    CHECK_NOTHROW(up::extractZipEntry(sample, "ShadeTube.exe", out / "nested.exe"));
    CHECK(readAll(out / "nested.exe") == "not really an exe");
    CHECK_THROWS(up::extractZipEntry(sample, "missing.txt", out / "missing.txt"), "Pakette missing.txt yok");

    // Hand-made stored zip with a trailing archive comment; the shallowest match wins.
    writeAll(out / "stored.zip", storedZip({{"deep/ShadeTube.exe", "deep"}, {"ShadeTube.exe", "top"}}, "archive comment"));
    CHECK_NOTHROW(up::extractZipEntry(out / "stored.zip", "ShadeTube.exe", out / "stored.exe"));
    CHECK(readAll(out / "stored.exe") == "top");
    std::string corrupt = storedZip({{"ShadeTube.exe", "payload"}});
    corrupt[30 + 13] = 'X';   // first data byte of the entry
    writeAll(out / "corrupt.zip", corrupt);
    CHECK_THROWS(up::extractZipEntry(out / "corrupt.zip", "ShadeTube.exe", out / "corrupt.exe"), "CRC-32");
    writeAll(out / "truncated.zip", readAll(sample).substr(0, 4000));
    CHECK_THROWS(up::extractZipEntry(out / "truncated.zip", "hello.txt", out / "t.txt"), "Paket bozuk");
    writeAll(out / "notzip.zip", "just text");
    CHECK_THROWS(up::extractZipEntry(out / "notzip.zip", "hello.txt", out / "t.txt"), "Paket bozuk");

    if (distZipPath.empty()) {
        std::printf("  skip  no dist\\ShadeTube-<ver>-win64.zip (run tools\\package.ps1 for the real-package checks)\n");
        return;
    }
    std::printf("        real package: %s\n", distZipPath.string().c_str());
    const fs::path exe = root / "dist" / "ShadeTube.exe";
    fs::create_directories(exe.parent_path());
    CHECK_NOTHROW(up::extractZipEntry(distZipPath, "ShadeTube.exe", exe));
    const fs::path staged = distZipPath.parent_path() / distZipPath.stem() / "ShadeTube.exe";
    if (present(staged)) CHECK(sameContent(exe, staged));
    CHECK(up::fileProductName(exe) == "ShadeTube");
    CHECK(up::normalizeVersion(up::fileProductVersion(exe)) == distVersion);
    CHECK_NOTHROW(up::verifyExe(exe, distVersion));
    CHECK_NOTHROW(up::verifyExe(exe, "v" + distVersion));
    CHECK_THROWS(up::verifyExe(exe, "99.0.0"), "beklenen 99.0.0");
    *appExe = exe;
}

void testVerify(const fs::path& root) {
    section("verifyExe");
    writeAll(root / "verify" / "text.exe", "MZ but nothing else");
    CHECK_THROWS(up::verifyExe(root / "verify" / "text.exe", "0.3.0"), "64 bit");
    // This console test exe is a PE32+ but not a GUI app named ShadeTube.
    CHECK_THROWS(up::verifyExe(up::currentExe(), "0.3.0"), "64 bit");
    CHECK_THROWS(up::verifyExe(root / "verify" / "missing.exe", "0.3.0"), "64 bit");
}

bool recorded(const fs::path& p) {
    const auto list = up::recordedLeftovers();
    return std::any_of(list.begin(), list.end(), [&](const fs::path& e) { return _wcsicmp(e.c_str(), p.c_str()) == 0; });
}

// Files a user keeps next to a portable exe with names close to the updater's: never deleted or overwritten.
const wchar_t* const kDecoys[] = {L"ShadeTube.older.exe", L"ShadeTube.old - Kopya.exe", L"ShadeTube.new.exe", L"ShadeTube.update.zip",
                                  L"ShadeTube.old-x.exe"};

void writeDecoys(const fs::path& dir) {
    for (const wchar_t* d : kDecoys) writeAll(dir / d, st::toUtf8(d));
}

bool decoysIntact(const fs::path& dir) {
    for (const wchar_t* d : kDecoys)
        if (readAll(dir / d) != st::toUtf8(d)) return false;
    return true;
}

size_t fileCount(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        (void)e;
        ++n;
    }
    return n;
}

void testSwap(const fs::path& root, const fs::path& appExe) {
    section("swap / rollback / recorded cleanup");
    const fs::path self = up::currentExe();
    const fs::path replacementSource = appExe.empty() ? fs::path(ST_UPDATER_FIXTURES) / "sample.zip" : appExe;

    CHECK(up::isUpdaterFileName(L"ShadeTube.old.exe") && up::isUpdaterFileName(L"shadetube.OLD-12.exe"));
    CHECK(up::isUpdaterFileName(L"ShadeTube.new-123-456.exe") && up::isUpdaterFileName(L"ShadeTube.update-1-2.zip"));
    for (const wchar_t* d : kDecoys) CHECK(!up::isUpdaterFileName(d));
    CHECK(!up::isUpdaterFileName(L"ShadeTube.new--1.exe") && !up::isUpdaterFileName(L"ShadeTube.old-.exe"));

    // Plain swap next to a user's own ShadeTube.old.exe (a kept fallback build) and look-alike files.
    const fs::path dir = root / "swap";
    const fs::path target = dir / "ShadeTube.exe", fresh = dir / "replacement.bin";
    fs::create_directories(dir);
    fs::copy_file(self, target, fs::copy_options::overwrite_existing);
    fs::copy_file(replacementSource, fresh, fs::copy_options::overwrite_existing);
    writeAll(dir / "ShadeTube.old.exe", "the user's fallback build");
    writeDecoys(dir);
    CHECK(!up::cleanupAfterUpdate(target, 0));   // nothing recorded: nothing deleted, even at startup
    CHECK(readAll(dir / "ShadeTube.old.exe") == "the user's fallback build" && decoysIntact(dir));
    fs::path parked;
    CHECK_NOTHROW(parked = up::swapExe(target, fresh));
    CHECK(parked == dir / "ShadeTube.old-2.exe");   // the user's ShadeTube.old.exe is taken: next free name
    CHECK(sameContent(target, replacementSource));
    CHECK(sameContent(parked, self));
    CHECK(recorded(parked));
    CHECK(!present(fresh));
    CHECK(up::cleanupAfterUpdate(target, 2000));
    CHECK(!present(parked) && !recorded(parked));
    CHECK(readAll(dir / "ShadeTube.old.exe") == "the user's fallback build");
    CHECK(decoysIntact(dir));
    CHECK(!up::cleanupAfterUpdate(target, 0));   // nothing left to do

    // Swap while the target is running (the real update case): the running image can be renamed, not deleted, so
    // cleanupAfterUpdate retries until the process has exited. A running, unrecorded ShadeTube.old.exe is left alone.
    const fs::path runDir = root / "running";
    const fs::path running = runDir / "ShadeTube.exe";
    fs::create_directories(runDir);
    fs::copy_file(self, running, fs::copy_options::overwrite_existing);
    fs::copy_file(self, runDir / "ShadeTube.old.exe", fs::copy_options::overwrite_existing);
    HANDLE staleProc = start(runDir / "ShadeTube.old.exe", L"--sleep 4000");
    HANDLE proc = start(running, L"--sleep 4000");
    CHECK(proc != nullptr && staleProc != nullptr);
    Sleep(300);
    fs::copy_file(replacementSource, runDir / "incoming.bin", fs::copy_options::overwrite_existing);
    CHECK_NOTHROW(parked = up::swapExe(running, runDir / "incoming.bin"));
    CHECK(parked == runDir / "ShadeTube.old-2.exe");
    CHECK(sameContent(running, replacementSource));
    CHECK(sameContent(parked, self));
    const ULONGLONG t0 = GetTickCount64();
    CHECK(up::cleanupAfterUpdate(running, 20000));   // returns once the parked exe's process has exited
    std::printf("        cleanup waited %llu ms for the old process\n", GetTickCount64() - t0);
    CHECK(!present(parked) && !recorded(parked));
    CHECK(present(runDir / "ShadeTube.old.exe"));    // unrecorded: never touched
    CHECK(fileCount(runDir) == 2);
    finish(proc);
    finish(staleProc);

    // Rollback: the replacement is locked (no FILE_SHARE_DELETE), so it cannot be moved in -> the original returns and
    // nothing stays recorded.
    const fs::path rbDir = root / "rollback";
    const fs::path rbTarget = rbDir / "ShadeTube.exe", rbNew = rbDir / "incoming.bin";
    fs::create_directories(rbDir);
    fs::copy_file(self, rbTarget, fs::copy_options::overwrite_existing);
    fs::copy_file(replacementSource, rbNew, fs::copy_options::overwrite_existing);
    const HANDLE lock = CreateFileW(rbNew.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK_THROWS(up::swapExe(rbTarget, rbNew), "mevcut sürüm korundu");
    CloseHandle(lock);
    CHECK(sameContent(rbTarget, self));
    CHECK(!present(rbDir / "ShadeTube.old.exe") && !recorded(rbDir / "ShadeTube.old.exe"));
    CHECK(present(rbNew));
    // Rollback when the replacement vanished.
    fs::remove(rbNew);
    CHECK_THROWS(up::swapExe(rbTarget, rbNew), "mevcut sürüm korundu");
    CHECK(sameContent(rbTarget, self));
    CHECK(fileCount(rbDir) == 1);
    CHECK(up::recordedLeftovers().empty());
}

void testDeferredDelete(const fs::path& root) {
    section("deleteAfterExit");
    // '%i' is the helper's FOR variable and '%PATH%' / '!x!' would expand in parsed cmd text: the paths travel as
    // environment variables, so the real folder goes and the look-alike sibling ("%i" -> "1") stays.
    const fs::path dir = root / "deferred %i %PATH% !x! (y)";
    const fs::path sibling = root / "deferred 1 %PATH% !x! (y)";
    writeAll(dir / "locked.bin", "x");
    writeAll(dir / "sub" / "b.txt", "y");
    writeAll(sibling / "keep.txt", "keep");
    const fs::path file = root / "deferred-file %TEMP%.txt";
    writeAll(file, "z");
    const HANDLE h = CreateFileW((dir / "locked.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(ins::deleteAfterExit({dir, file}));
    wchar_t probe[8];
    CHECK(GetEnvironmentVariableW(L"ST_DEL_1", probe, 8) == 0);   // not left in this process's environment
    Sleep(2500);
    CHECK(present(dir / "locked.bin"));   // still held open: the helper keeps retrying
    CloseHandle(h);
    CHECK(waitFor([&] { return !present(dir); }, 15000));
    CHECK(waitFor([&] { return !present(file); }, 15000));
    CHECK(readAll(sibling / "keep.txt") == "keep");
}

void testRelaunch(const fs::path& root) {
    section("launchAfterExit");
    const fs::path marker = root / "relaunch.marker";
    HANDLE child = start(up::currentExe(), L"--relaunch \"" + marker.wstring() + L"\"");
    CHECK(child != nullptr);
    finish(child);
    CHECK(waitFor([&] { return present(marker) && !readAll(marker).empty(); }, 15000));
    CHECK(readAll(marker) == "mutex-free");   // started after main() returned (and closed the mutex)
}

// The visible dialog (#32770) of process `pid` other than `except`, waiting up to `timeoutMs`.
HWND dialogOf(DWORD pid, HWND except, int timeoutMs) {
    struct S {
        DWORD pid;
        HWND except, found;
    } s{pid, except, nullptr};
    waitFor(
        [&] {
            s.found = nullptr;
            EnumWindows(
                [](HWND w, LPARAM p) -> BOOL {
                    auto& st = *reinterpret_cast<S*>(p);
                    DWORD owner = 0;
                    GetWindowThreadProcessId(w, &owner);
                    wchar_t cls[32];
                    if (owner == st.pid && w != st.except && IsWindowVisible(w) && GetClassNameW(w, cls, 32) && wcscmp(cls, L"#32770") == 0) {
                        st.found = w;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&s));
            return s.found != nullptr;
        },
        timeoutMs);
    return s.found;
}

// `--uninstall` without --quiet: ticks "Ayarları ... de sil" (when asked), presses "Kaldır", then OK on the report.
bool driveUninstallDialogs(HANDLE proc, bool removeData) {
    if (!proc) return false;
    const DWORD pid = GetProcessId(proc);
    const HWND confirm = dialogOf(pid, nullptr, 15000);
    if (!confirm) return false;
    Sleep(300);
    if (removeData) SendMessageW(confirm, TDM_CLICK_VERIFICATION, TRUE, FALSE);
    SendMessageW(confirm, TDM_CLICK_BUTTON, 100, 0);   // "Kaldır"
    const HWND report = dialogOf(pid, confirm, 60000);   // after closing other instances (up to 30 s each)
    if (!report) return false;
    Sleep(300);
    SendMessageW(report, TDM_CLICK_BUTTON, IDOK, 0);
    return true;
}

void testInstaller(const fs::path& root, const fs::path& appExe) {
    section("installer (overridden locations)");
    const fs::path installDir = root / "Programs" / "ShadeTube", links = root / "Start Menu" / "Programs";
    SetEnvironmentVariableW(L"SHADETUBE_INSTALL_DIR", installDir.c_str());
    SetEnvironmentVariableW(L"SHADETUBE_SHORTCUT_DIR", links.c_str());
    SetEnvironmentVariableW(L"SHADETUBE_UNINSTALL_KEY", L"HKEY_CURRENT_USER\\Software\\ShadeTubeTest\\Uninstall\\ShadeTube\\");
    const ins::Locations loc = ins::locations();
    const bool safe = loc.uninstallKey == L"Software\\ShadeTubeTest\\Uninstall\\ShadeTube" && loc.dir == installDir &&
                      loc.shortcut == links / L"ShadeTube.lnk" && loc.exe == installDir / L"ShadeTube.exe";
    CHECK(safe);
    if (!safe) {
        std::printf("  skip  installer tests: the overrides did not apply, refusing to touch real locations\n");
        return;
    }
    SetEnvironmentVariableW(L"SHADETUBE_UNINSTALL_KEY", L"HKCU");
    CHECK(ins::locations().uninstallKey == L"Software\\ShadeTube\\InvalidUninstallKeyOverride");   // never the real key
    SetEnvironmentVariableW(L"SHADETUBE_UNINSTALL_KEY", L"HKCU\\Software\\ShadeTubeTest\\Uninstall\\ShadeTube");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ShadeTubeTest");   // leftovers of an aborted run

    const fs::path self = root / "selfcopy" / "updater_test.exe";
    fs::create_directories(self.parent_path());
    fs::copy_file(up::currentExe(), self, fs::copy_options::overwrite_existing);
    const fs::path app = appExe.empty() ? self : appExe;
    const fs::path data = root / "data";
    writeAll(data / "settings.json", "{}");

    CHECK(!ins::installedExe());
    CHECK_NOTHROW(ins::install(app));
    CHECK(sameContent(loc.exe, app));
    CHECK(!present(installDir / L"ShadeTube.exe.tmp"));
    const fs::path target = ins::shortcutTarget(loc.shortcut);
    std::printf("        shortcut -> %s\n", target.string().c_str());
    CHECK(!target.empty() && fs::equivalent(target, loc.exe));
    const ins::Registration r = ins::readRegistration();
    const std::wstring quoted = L"\"" + loc.exe.wstring() + L"\"";
    CHECK(r.exists);
    CHECK(r.displayName == L"ShadeTube");
    CHECK(r.displayVersion == st::toWide(up::normalizeVersion(up::fileProductVersion(app))));
    std::printf("        DisplayVersion = %s\n", st::toUtf8(r.displayVersion).c_str());
    CHECK(r.publisher == L"ShadeTube");
    CHECK(r.displayIcon == loc.exe.wstring() + L",0");
    CHECK(r.installLocation == installDir.wstring());
    CHECK(r.uninstallString == quoted + L" --uninstall");
    CHECK(r.quietUninstallString == quoted + L" --uninstall --quiet");
    CHECK(r.estimatedSizeKb == static_cast<uint32_t>((fs::file_size(app) + 1023) / 1024));
    CHECK(r.noModify == 1 && r.noRepair == 1);
    CHECK(ins::installedExe() && fs::equivalent(*ins::installedExe(), loc.exe));
    CHECK(!ins::runningInstalled());

    // Reinstall while the installed exe is running (another session): renamed away instead of overwritten.
    CHECK_NOTHROW(ins::install(self));
    HANDLE proc = start(loc.exe, L"--sleep 5000");
    CHECK(proc != nullptr);
    Sleep(300);
    CHECK_NOTHROW(ins::install(app));
    CHECK(sameContent(loc.exe, app));
    CHECK(present(installDir / L"ShadeTube.old.exe"));

    // Uninstall while that old exe still runs: registration + shortcut go now, the folder once it has exited.
    CHECK_NOTHROW(ins::uninstall(false));
    CHECK(!present(loc.shortcut));
    CHECK(!ins::readRegistration().exists);
    CHECK(!ins::installedExe());
    CHECK(present(installDir));   // held by the running process
    finish(proc);
    CHECK(waitFor([&] { return !present(installDir); }, 20000));
    CHECK(present(data / "settings.json"));   // user data kept

    // Foreign files in the folder are never deleted.
    CHECK_NOTHROW(ins::install(app));
    writeAll(installDir / L"notlar.txt", "kullanıcı dosyası");
    CHECK_NOTHROW(ins::uninstall(false));
    CHECK(present(installDir / L"notlar.txt"));
    CHECK(!present(loc.exe));
    CHECK(!ins::readRegistration().exists && !present(loc.shortcut));
    fs::remove_all(installDir);

    // Uninstall with the user data (SHADETUBE_DATA_DIR): the data folder goes via the helper.
    CHECK_NOTHROW(ins::install(app));
    CHECK_NOTHROW(ins::uninstall(true));
    CHECK(!present(installDir));   // not in use: removed at once
    CHECK(waitFor([&] { return !present(data); }, 20000));
    CHECK(!ins::readRegistration().exists && !present(loc.shortcut));

    CHECK_NOTHROW(ins::unregister());   // idempotent

    // `ShadeTube.exe --uninstall --quiet` (what Windows runs; this exe forwards to runUninstallCommand() like
    // main.cpp): a ShadeTube running from the install folder is asked to quit (gracefully, WM_QUIT), then removed.
    // The instance must be this tree's build (it honours SHADETUBE_DATA_DIR); never the packaged exe, which may be
    // an older release that would read and rewrite the user's real %LOCALAPPDATA%\ShadeTube.
    const fs::path liveApp = fs::path(ST_APP_EXE);
    if (!appExe.empty() && fs::exists(liveApp)) {
        fs::create_directories(data);
        writeAll(data / "settings.json", "{}");
        CHECK_NOTHROW(ins::install(liveApp));
        const fs::path appProfile = root / "installed-profile";
        SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", appProfile.c_str());   // the app instance's own profile
        HANDLE appProc = start(loc.exe, L"--preview --route settings");
        SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", data.c_str());
        CHECK(appProc != nullptr);
        const DWORD appPid = appProc ? GetProcessId(appProc) : 0;
        auto hasWindow = [&] {
            struct S {
                DWORD pid;
                bool found;
            } s{appPid, false};
            EnumWindows(
                [](HWND w, LPARAM p) -> BOOL {
                    auto& st = *reinterpret_cast<S*>(p);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(w, &pid);
                    wchar_t cls[64];
                    if (pid == st.pid && GetClassNameW(w, cls, 64) && wcscmp(cls, L"ShadeTube.Window") == 0) st.found = true;
                    return !st.found;
                },
                reinterpret_cast<LPARAM>(&s));
            return s.found;
        };
        CHECK(waitFor(hasWindow, 20000));
        HANDLE cmd = start(up::currentExe(), L"--uninstall --quiet");
        CHECK(cmd != nullptr);
        DWORD cmdExit = 99;
        if (cmd) {
            WaitForSingleObject(cmd, 60000);
            GetExitCodeProcess(cmd, &cmdExit);
            CloseHandle(cmd);
        }
        CHECK(cmdExit == 0);
        DWORD appExit = 99;
        CHECK(appProc && WaitForSingleObject(appProc, 1000) == WAIT_OBJECT_0);   // quit before the command returned
        if (appProc) {
            GetExitCodeProcess(appProc, &appExit);
            CloseHandle(appProc);
        }
        std::printf("        installed app exit code %lu (0 = graceful WM_QUIT)\n", appExit);
        CHECK(appExit == 0);
        CHECK(!ins::readRegistration().exists && !present(loc.shortcut));
        CHECK(waitFor([&] { return !present(installDir); }, 20000));
        CHECK(present(data / "settings.json"));   // --quiet keeps the user data
        std::error_code ec;
        fs::remove_all(appProfile, ec);
    }

    // Windows "Kaldır" + "Ayarları ... de sil" while another ShadeTube runs from a different folder: it is asked to quit
    // before the shared data folder goes; one that stays keeps the data. Fake instances use a test-only window class
    // (SHADETUBE_INSTANCE_CLASS), so real ShadeTube windows on this desktop are never touched.
    const std::wstring cls = L"ShadeTube.TestInstance." + std::to_wstring(GetCurrentProcessId());
    const std::wstring mutex = L"Local\\ShadeTube.TestMutex." + std::to_wstring(GetCurrentProcessId());   // never held
    SetEnvironmentVariableW(L"SHADETUBE_INSTANCE_CLASS", cls.c_str());
    SetEnvironmentVariableW(L"SHADETUBE_INSTANCE_MUTEX", mutex.c_str());
    for (const bool stubborn : {false, true}) {
        std::printf("        other instance %s\n", stubborn ? "ignores WM_QUIT (data must stay)" : "quits (data removed)");
        writeAll(data / "settings.json", "{}");
        CHECK_NOTHROW(ins::install(app));
        HANDLE fake = start(up::currentExe(), L"--fake-instance " + cls + (stubborn ? L" stubborn" : L" quits"));
        CHECK(fake != nullptr);
        CHECK(waitFor([&] { return FindWindowW(cls.c_str(), nullptr) != nullptr; }, 10000));
        HANDLE cmd = start(up::currentExe(), L"--uninstall");
        CHECK(cmd != nullptr);
        CHECK(driveUninstallDialogs(cmd, true));
        DWORD cmdExit = 99;
        if (cmd) {
            WaitForSingleObject(cmd, 60000);
            GetExitCodeProcess(cmd, &cmdExit);
            CloseHandle(cmd);
        }
        CHECK(cmdExit == 0);
        CHECK(!ins::readRegistration().exists && !present(loc.shortcut));
        CHECK(waitFor([&] { return !present(installDir); }, 20000));
        if (!stubborn) {
            CHECK(fake && WaitForSingleObject(fake, 1000) == WAIT_OBJECT_0);   // asked to quit before the data went
            CHECK(waitFor([&] { return !present(data); }, 20000));
        } else {
            CHECK(fake && WaitForSingleObject(fake, 0) == WAIT_TIMEOUT);       // still running...
            Sleep(3000);
            CHECK(present(data / "settings.json"));                          // ...so the data was kept
            if (fake) TerminateProcess(fake, 0);
        }
        finish(fake);
    }
    SetEnvironmentVariableW(L"SHADETUBE_INSTANCE_CLASS", nullptr);
    SetEnvironmentVariableW(L"SHADETUBE_INSTANCE_MUTEX", nullptr);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ShadeTubeTest");
    HKEY k = nullptr;
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ShadeTubeTest", 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND);
    if (k) RegCloseKey(k);
}

// ---- End to end against the mock server (run_e2e.ps1 prepares it) ---------------------------------------------

void setUpdateUrl(const std::string& base, const char* path) {
    SetEnvironmentVariableW(L"SHADETUBE_UPDATE_URL", st::toWide(base + path).c_str());
}

void testE2e(const std::string& base) {
    const fs::path root = tempRoot();
    const fs::path self = up::currentExe();
    fs::create_directories(root);

    section("check (mock GitHub API)");
    setUpdateUrl(base, "/releases/latest");
    CHECK(up::updateUrl() == base + "/releases/latest");
    up::CheckResult c;
    CHECK_NOTHROW(c = up::check("0.2.0"));
    CHECK(c.status == up::CheckStatus::Available);
    const up::Release release = c.release;
    std::printf("        latest %s, asset %s\n", release.version.c_str(), up::pickAsset(release) ? up::pickAsset(release)->name.c_str() : "-");
    CHECK(up::pickAsset(release) != nullptr);
    CHECK_NOTHROW(c = up::check(release.version));
    CHECK(c.status == up::CheckStatus::UpToDate);
    CHECK_NOTHROW(c = up::check("99.0.0"));
    CHECK(c.status == up::CheckStatus::UpToDate);
    setUpdateUrl(base, "/missing/releases/latest");
    CHECK_NOTHROW(c = up::check("0.2.0"));
    CHECK(c.status == up::CheckStatus::NoReleases);
    setUpdateUrl(base, "/broken/releases/latest");
    CHECK_THROWS(up::check("0.2.0"), "Sürüm bilgisi okunamadı");
    SetEnvironmentVariableW(L"SHADETUBE_UPDATE_URL", L"http://127.0.0.1:1/releases/latest");
    CHECK_THROWS(up::check("0.2.0"), "GitHub'a ulaşılamadı");

    auto fetch = [&](const char* path) {
        setUpdateUrl(base, path);
        return up::check("0.2.0").release;
    };
    // One install into a fresh copy of this exe, next to a user's look-alike files; returns the progress calls.
    // cancelAt: -1 never, 0 before the start, n = from inside the n-th progress call (mid-download).
    auto installInto = [&](const char* name, const up::Release& rel, int cancelAt, bool expectOk, const char* needle) {
        const fs::path dir = root / "e2e" / name;
        const fs::path target = dir / "ShadeTube.exe";
        fs::create_directories(dir);
        fs::copy_file(self, target, fs::copy_options::overwrite_existing);
        writeDecoys(dir);
        YoutubeExplode::CancellationTokenSource cancel;
        if (cancelAt == 0) cancel.cancel();
        int calls = 0;
        int64_t lastDone = 0, lastTotal = 0;
        auto progress = [&](int64_t done, int64_t total) {
            ++calls;
            lastDone = done;
            lastTotal = total;
            if (calls == cancelAt) cancel.cancel();
        };
        if (expectOk) {
            CHECK_NOTHROW(up::downloadAndInstall(rel, target, progress, cancel.token()));
            CHECK(up::fileProductName(target) == "ShadeTube");
            CHECK(up::normalizeVersion(up::fileProductVersion(target)) == rel.version);
            CHECK(sameContent(dir / "ShadeTube.old.exe", self));
            CHECK(recorded(dir / "ShadeTube.old.exe"));
            CHECK(lastDone == lastTotal && lastTotal > 0);
            CHECK(fileCount(dir) == 2 + std::size(kDecoys));   // exe + parked exe + the user's files
            CHECK(up::cleanupAfterUpdate(target, 2000));
            CHECK(!present(dir / "ShadeTube.old.exe"));
        } else {
            CHECK_THROWS(up::downloadAndInstall(rel, target, progress, cancel.token()), needle);
            CHECK(sameContent(target, self));   // untouched
            CHECK(fileCount(dir) == 1 + std::size(kDecoys));   // no temp file left
        }
        CHECK(decoysIntact(dir));
        const auto left = up::recordedLeftovers();
        CHECK(std::none_of(left.begin(), left.end(), [&](const fs::path& p) { return p.parent_path() == dir; }));
        std::printf("        %s: %d progress call(s), %lld bytes\n", name, calls, static_cast<long long>(lastDone));
        return calls;
    };

    section("download + verify + swap: zip, ranged");
    CHECK(installInto("zip-ranged", release, -1, true, "") > 1);   // 1 MB chunks
    section("download + verify + swap: zip, server ignores Range");
    CHECK(installInto("zip-norange", fetch("/norange/releases/latest"), -1, true, "") == 1);
    section("download + verify + swap: raw exe asset");
    installInto("exe", fetch("/exe/releases/latest"), -1, true, "");
    section("rejected: SHA-256 in the release notes does not match");
    installInto("badsha", fetch("/badsha/releases/latest"), -1, false, "SHA-256");
    section("rejected: asset digest does not match");
    installInto("baddigest", fetch("/baddigest/releases/latest"), -1, false, "SHA-256");
    section("rejected: the exe in the zip is not the tagged version");
    installInto("wrongversion", fetch("/wrongversion/releases/latest"), -1, false, "beklenen 99.0.0");
    section("rejected: missing asset (404)");
    installInto("missingasset", fetch("/missingasset/releases/latest"), -1, false, "HTTP 404");
    section("cancelled before the start");
    installInto("cancelled", release, 0, false, "iptal");
    section("cancelled mid-download (quitting: the persist hook cancels)");
    installInto("cancelled-mid", release, 1, false, "iptal");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Child modes. --uninstall [--quiet] forwards exactly like the main.cpp integration does.
    if (ins::uninstallRequested()) return ins::runUninstallCommand();
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--sleep") {
        Sleep(static_cast<DWORD>(_wtoi(argv[2])));
        return 0;
    }
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--relaunch") {
        // Like main.cpp: a named mutex held until right before returning; the relaunch must not see it.
        const std::wstring mutex = L"Local\\ShadeTube.UpdaterTest." + std::to_wstring(GetCurrentProcessId());
        const HANDLE m = CreateMutexW(nullptr, TRUE, mutex.c_str());
        ins::launchAfterExit(up::currentExe(), L"--write-marker \"" + std::wstring(argv[2]) + L"\" " + mutex);
        Sleep(700);
        if (m) CloseHandle(m);
        return 0;
    }
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--fake-instance") {
        // Stand-in for a ShadeTube running from another folder: a hidden window of the given class. "quits" ends on
        // WM_QUIT like App::run; "stubborn" ignores it (and ends by itself after 60 s).
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = argv[2];
        RegisterClassW(&wc);
        const HWND w = CreateWindowExW(0, argv[2], L"ShadeTube", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
        const bool stubborn = std::wstring_view(argv[3]) == L"stubborn";
        const ULONGLONG end = GetTickCount64() + 60000;
        MSG msg;
        while (GetTickCount64() < end) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT && !stubborn) {
                    DestroyWindow(w);
                    return 0;
                }
                DispatchMessageW(&msg);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        DestroyWindow(w);
        return 0;
    }
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--write-marker") {
        const HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, argv[3]);
        writeAll(argv[2], m ? "mutex-held" : "mutex-free");
        if (m) CloseHandle(m);
        return 0;
    }

    const fs::path root = tempRoot();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    // Never the user's profile: paths::appData() (user data removal) resolves here.
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (root / "data").c_str());

    if (argc >= 3 && std::wstring_view(argv[1]) == L"--e2e") {
        testE2e(st::toUtf8(argv[2]));
    } else {
        std::string distVersion;
        const fs::path dist = distZip(&distVersion);
        fs::path appExe;
        testVersions();
        testRelease();
        testZip(root, dist, distVersion, &appExe);
        testVerify(root);
        testSwap(root, appExe);
        testDeferredDelete(root);
        testRelaunch(root);
        testInstaller(root, appExe);
    }

    fs::remove_all(root, ec);
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
