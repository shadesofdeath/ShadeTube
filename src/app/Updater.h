#pragma once
// Self-update from GitHub releases. UI-independent (the Ayarlar › HAKKINDA rows, the startup check and the relaunch
// live in app/AboutSettings.cpp); everything here may run on a worker thread.
//
//   check    GET <releases/latest> (GitHub API; SHADETUBE_UPDATE_URL overrides it for tests) -> Release.
//            404 = no public release yet (reported as NoReleases, not as an error).
//   install  download the asset (ShadeTube-<ver>-win64.zip as built by tools/package.ps1, or a raw .exe) in ranged
//            chunks -> extract ShadeTube.exe with the built-in ZIP reader (stored / deflate, CRC-32 checked) next to
//            the target -> verify it (PE x64 GUI exe, VERSIONINFO ProductName "ShadeTube" and ProductVersion ==
//            release version, SHA-256 from the asset digest and/or a "sha256: <hex>" line of the release notes) ->
//            swap: the target is renamed to the first free ShadeTube.old[-n].exe, the new exe takes its name (rolled
//            back when the second rename fails, so a half-replaced exe is never left behind).
//   cleanup  every file the updater creates or parks (unique temp names created with CREATE_NEW, the parked old
//            exe) is recorded in paths::appData()\update-leftovers.txt BEFORE it exists; the next start deletes
//            exactly those recorded files (cleanupAfterUpdate) and never anything else next to the exe.
// User-facing failures are thrown as std::runtime_error with a message in the UI language (UTF-8; shown in a toast
// as is).
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::updater {

// ---- Versions ---------------------------------------------------------------------------------------
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;   // pre-release identifiers ("beta.1"); empty = a release
};
// "0.4.0", "v0.4.0", "0.4" (= 0.4.0), "0.4.0-beta.1", "0.4.0+build.7" (build metadata ignored). nullopt = not a version.
std::optional<Version> parseVersion(std::string_view s);
// Semver precedence: < 0 when a is older than b, 0 when equal, > 0 when newer. Unparsable = 0.0.0.
int compareVersions(std::string_view a, std::string_view b);
// "v0.4" -> "0.4.0"; an unparsable tag is returned trimmed, without a leading 'v'.
std::string normalizeVersion(std::string_view tag);

// VERSIONINFO string values of an exe (first translation, then en-US). Empty when missing.
std::string fileProductVersion(const std::filesystem::path& exe);
std::string fileProductName(const std::filesystem::path& exe);
std::filesystem::path currentExe();   // the running exe (GetModuleFileNameW)
// ProductVersion of the running exe ("0.3.0"). SHADETUBE_VERSION_OVERRIDE replaces it: tests only, to pretend an
// older build against a mock release.
const std::string& currentVersion();

// ---- Releases ---------------------------------------------------------------------------------------
struct Asset {
    std::string name, url;   // url = browser_download_url
    int64_t size = 0;
    std::string sha256;      // from the asset's "digest" ("sha256:<hex>", GitHub adds it), lowercase; may be empty
};
struct Release {
    std::string tag, version;   // version = normalizeVersion(tag)
    std::string name, body, htmlUrl, publishedAt;
    std::vector<Asset> assets;
    std::vector<std::string> notesSha256;   // every "sha256: <hex>" of the release notes, lowercase
};
Release parseRelease(std::string_view json);   // throws on malformed JSON / a missing tag_name
// ShadeTube-<ver>-win64.zip, else any ShadeTube *.zip, else a ShadeTube *.exe. nullptr when there is none.
const Asset* pickAsset(const Release& r);
std::vector<std::string> findSha256(std::string_view text);   // 64-hex tokens on lines that mention sha256
std::string updateUrl();   // SHADETUBE_UPDATE_URL, else the GitHub API latest-release URL
std::string userAgent();   // "ShadeTube/<ver> (+https://github.com/shadesofdeath/ShadeTube)"

enum class CheckStatus { UpToDate, Available, NoReleases };
struct CheckResult {
    CheckStatus status = CheckStatus::UpToDate;
    Release release;   // Available / UpToDate
};
// One request (worker thread). Throws std::runtime_error (UI language) on network / HTTP / parse errors.
CheckResult check(std::string_view runningVersion);

// ---- Download / verify / swap -----------------------------------------------------------------------
struct Cancelled : std::runtime_error {
    Cancelled();   // "Güncelleme iptal edildi" in the UI language
};
using ProgressFn = std::function<void(int64_t done, int64_t total)>;   // total < 0: unknown
using CancelToken = YoutubeExplode::CancellationToken;   // from a CancellationTokenSource; aborts in-flight reads too
// Ranged GETs (1 MB) so progress can be reported; a server that ignores Range (200) is handled too. Writes into `out`
// (truncating it): pass only a file the caller created. Throws Cancelled once `cancel` is set.
void download(const std::string& url, const std::filesystem::path& out, const ProgressFn& progress, const CancelToken& cancel);
std::string sha256File(const std::filesystem::path& file);   // lowercase hex
uint32_t crc32(const void* data, size_t size, uint32_t crc = 0);
// Raw deflate (RFC 1951) of exactly `expectedSize` output bytes. Throws on corrupt input.
std::string inflate(std::string_view compressed, size_t expectedSize);
// Extracts the entry whose file name (any folder) equals `fileName` (case-insensitive) to `out`. Stored / deflate,
// CRC-32 checked; no ZIP64 / encryption. Throws std::runtime_error.
void extractZipEntry(const std::filesystem::path& zip, std::string_view fileName, const std::filesystem::path& out);
// PE32+ x64 GUI executable whose VERSIONINFO says ProductName "ShadeTube" and ProductVersion == expectedVersion.
void verifyExe(const std::filesystem::path& exe, std::string_view expectedVersion);
// target -> the first free <dir>\ShadeTube.old.exe / ShadeTube.old-<n>.exe (recorded as a leftover first), then
// replacement -> target. Works while `target` is running; never deletes or overwrites an existing file. On failure the
// original is restored and std::runtime_error is thrown. Returns where the previous exe was parked.
std::filesystem::path swapExe(const std::filesystem::path& target, const std::filesystem::path& replacement);
// Download + extract + verify + swap for `target` (the running exe in the app, a copy in tests). The last progress
// call has done == total; extraction and verification follow (UI: "Doğrulanıyor…"). Temporary files get unique names
// (ShadeTube.new-<pid>-<tick>.exe, ShadeTube.update-<pid>-<tick>.zip) created with CREATE_NEW and are removed again.
void downloadAndInstall(const Release& release, const std::filesystem::path& target, const ProgressFn& progress,
                        const CancelToken& cancel);

// ---- Leftover record (paths::appData()\update-leftovers.txt, one full path per line) ------------------
std::vector<std::filesystem::path> recordedLeftovers();
// Is `name` one of the names the updater creates (ShadeTube.old.exe, ShadeTube.old-<n>.exe,
// ShadeTube.new-<n>-<n>.exe, ShadeTube.update-<n>-<n>.zip)? Recorded paths are only deleted when this holds too.
bool isUpdaterFileName(const std::filesystem::path& name);
// Next start: deletes the recorded leftovers that exist (never `exe` itself), retrying up to `timeoutMs` while the
// previous process is still exiting; entries that are gone are dropped from the record, the rest stay for the next
// start. Nothing unrecorded is touched. Returns true when a recorded leftover was deleted.
bool cleanupAfterUpdate(const std::filesystem::path& exe, int timeoutMs);

} // namespace st::app::updater
