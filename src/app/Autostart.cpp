// "Windows ile başlat": the HKCU Run value. See Autostart.h.
#include "app/Autostart.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>

#include <cwctype>

namespace st::app::autostart {

namespace {

constexpr wchar_t kValueName[] = L"ShadeTube";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

std::wstring env(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

// SHADETUBE_RUN_KEY: a relative key with at least one separator and no empty segment (never the hive root).
std::wstring testKey() {
    std::wstring k = env(L"SHADETUBE_RUN_KEY");
    if (k.empty()) return {};
    if (k.find(L'\\') == std::wstring::npos || k.find(L"\\\\") != std::wstring::npos || k.front() == L'\\' || k.back() == L'\\')
        return L"Software\\ShadeTube\\InvalidRunKeyOverride";
    return k;
}

} // namespace

bool allowed() {
    if (!testKey().empty()) return true;
    const bool sandbox = !env(L"SHADETUBE_DATA_DIR").empty();
    const bool redirected = !env(L"SHADETUBE_INSTALL_DIR").empty() || !env(L"SHADETUBE_SHORTCUT_DIR").empty() ||
                            !env(L"SHADETUBE_UNINSTALL_KEY").empty();
    return !sandbox && !redirected;
}

std::wstring runKey() {
    const std::wstring t = testKey();
    return t.empty() ? std::wstring(kRunKey) : t;
}

std::wstring approvedKey() {
    const std::wstring t = testKey();
    return t.empty() ? std::wstring(kApprovedKey) : t + L"\\StartupApproved";
}

std::wstring commandFor(const std::filesystem::path& exe) { return L"\"" + exe.wstring() + L"\" " + kFlag; }

std::optional<Parsed> parseCommand(std::wstring_view cmd) {
    while (!cmd.empty() && std::iswspace(cmd.front())) cmd.remove_prefix(1);
    if (cmd.empty()) return std::nullopt;
    Parsed p;
    std::wstring_view rest;
    if (cmd.front() == L'"') {
        const size_t close = cmd.find(L'"', 1);
        p.exe = std::wstring(cmd.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1));
        rest = close == std::wstring_view::npos ? std::wstring_view{} : cmd.substr(close + 1);
    } else {
        const size_t space = cmd.find(L' ');
        p.exe = std::wstring(cmd.substr(0, space));
        rest = space == std::wstring_view::npos ? std::wstring_view{} : cmd.substr(space);
    }
    // Arguments: whitespace-separated tokens; the flag matches case-insensitively.
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && std::iswspace(rest[i])) ++i;
        const size_t start = i;
        while (i < rest.size() && !std::iswspace(rest[i])) ++i;
        if (i > start && CompareStringOrdinal(rest.data() + start, static_cast<int>(i - start), kFlag, -1, TRUE) == CSTR_EQUAL)
            p.autostartFlag = true;
    }
    return p;
}

std::optional<std::wstring> read() {
    if (!allowed()) return std::nullopt;
    DWORD bytes = 0;
    const std::wstring key = runKey();
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), kValueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring s(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(s.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), kValueName, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

bool write(const std::filesystem::path& exe) {
    if (!allowed() || exe.empty()) return false;
    const std::wstring cmd = commandFor(exe);
    const LSTATUS st = RegSetKeyValueW(HKEY_CURRENT_USER, runKey().c_str(), kValueName, REG_SZ, cmd.c_str(),
                                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    if (st != ERROR_SUCCESS) {
        ST_LOG_WARN("autostart", "Run value not written (error {})", static_cast<long>(st));
        return false;
    }
    ST_LOG_INFO("autostart", "Run value set for {}", toUtf8(exe.wstring()));
    return true;
}

bool remove() {
    if (!allowed()) return false;
    const LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, runKey().c_str(), kValueName);
    if (st == ERROR_SUCCESS) ST_LOG_INFO("autostart", "Run value removed");
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND || st == ERROR_PATH_NOT_FOUND;
}

bool disabledByWindows() {
    if (!allowed()) return false;
    BYTE data[16]{};
    DWORD bytes = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, approvedKey().c_str(), kValueName, RRF_RT_REG_BINARY, nullptr, data, &bytes) != ERROR_SUCCESS ||
        bytes == 0)
        return false;
    return (data[0] & 1) != 0;   // 02 / 06 = enabled, 03 / 07 = disabled by the user
}

void clearWindowsDisabled() {
    if (!allowed() || !disabledByWindows()) return;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, approvedKey().c_str(), kValueName);
    ST_LOG_INFO("autostart", "Windows' startup-apps switch cleared (turned on in Ayarlar)");
}

bool sync(bool enabled, const std::filesystem::path& exe) {
    if (!allowed()) return false;
    const auto cur = read();
    if (!enabled) {
        if (cur) remove();
        return false;
    }
    if (cur && *cur == commandFor(exe)) return true;
    return write(exe);
}

} // namespace st::app::autostart
