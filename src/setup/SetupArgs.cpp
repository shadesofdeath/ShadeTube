// ShadeTubeSetup.exe's command line. See SetupArgs.h.
#include "setup/SetupArgs.h"

#include "core/Utf.h"

#include <windows.h>

#include <cwctype>

namespace st::setup {

namespace {

bool iequals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool istarts(std::wstring_view s, std::wstring_view prefix) { return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix); }

std::wstring_view trim(std::wstring_view s) {
    while (!s.empty() && std::iswspace(s.front())) s.remove_prefix(1);
    while (!s.empty() && std::iswspace(s.back())) s.remove_suffix(1);
    return s;
}

std::wstring unquote(std::wstring_view s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return std::wstring(s);
}

} // namespace

std::wstring_view argumentsOf(std::wstring_view cmd) {
    cmd = trim(cmd);
    if (cmd.empty()) return {};
    size_t end = 0;
    if (cmd.front() == L'"') {
        end = cmd.find(L'"', 1);
        end = end == std::wstring_view::npos ? cmd.size() : end + 1;
    } else {
        while (end < cmd.size() && !std::iswspace(cmd[end])) ++end;
    }
    return trim(cmd.substr(end));
}

std::optional<Args> parseArgs(std::wstring_view s, std::wstring* error) {
    Args a;
    // Tokens: whitespace separated, a quoted token keeps its spaces ("C:\My Music"). /D= takes the rest of the line.
    std::vector<std::wstring> tokens;
    size_t i = 0;
    auto bad = [&](std::wstring_view what) -> std::optional<Args> {
        if (error) *error = std::wstring(what);
        return std::nullopt;
    };
    while (i < s.size()) {
        while (i < s.size() && std::iswspace(s[i])) ++i;
        if (i >= s.size()) break;
        const std::wstring_view rest = s.substr(i);
        // NSIS: "/D=<folder>" is the last switch and may contain spaces without quotes; quoted forms work too.
        if (istarts(rest, L"/D=") || istarts(rest, L"-D=") || istarts(rest, L"\"/D=") || istarts(rest, L"\"-D=")) {
            std::wstring_view v = trim(rest);
            if (v.front() == L'"') {   // "/D=C:\x y"
                v.remove_prefix(1);
                if (!v.empty() && v.back() == L'"') v.remove_suffix(1);
            }
            a.dir = unquote(v.substr(3));
            if (a.dir.empty()) return bad(L"/D=");
            break;
        }
        std::wstring token;
        if (s[i] == L'"') {
            const size_t close = s.find(L'"', i + 1);
            token = std::wstring(s.substr(i + 1, close == std::wstring_view::npos ? std::wstring_view::npos : close - i - 1));
            i = close == std::wstring_view::npos ? s.size() : close + 1;
        } else {
            const size_t start = i;
            while (i < s.size() && !std::iswspace(s[i])) ++i;
            token = std::wstring(s.substr(start, i - start));
        }
        tokens.push_back(std::move(token));
    }
    for (size_t t = 0; t < tokens.size(); ++t) {
        const std::wstring& k = tokens[t];
        auto value = [&]() -> std::optional<std::wstring> {
            if (t + 1 >= tokens.size()) return std::nullopt;
            return tokens[++t];
        };
        // Dev flags: two dashes.
        if (k.rfind(L"--", 0) == 0) {
            if (iequals(k, L"--screenshot")) {
                const auto v = value();
                if (!v || v->empty()) return bad(k);
                a.screenshotDir = *v;
            } else if (iequals(k, L"--theme")) {
                const auto v = value();
                if (v && iequals(*v, L"light")) a.light = true;
                else if (v && iequals(*v, L"dark")) a.light = false;
                else return bad(k);
            } else if (iequals(k, L"--scale")) {
                const auto v = value();
                const float f = v ? static_cast<float>(_wtof(v->c_str())) : 0.f;
                if (f < 0.5f || f > 4.f) return bad(k);
                a.scale = f;
            } else if (iequals(k, L"--lang")) {
                const auto v = value();
                if (!v || v->empty() || v->size() > 8) return bad(k);
                a.lang = toUtf8(*v);
            } else if (iequals(k, L"--page")) {
                const auto v = value();
                if (!v || v->empty()) return bad(k);
                a.page = toUtf8(*v);
            } else {
                return bad(k);
            }
            continue;
        }
        if (k.size() < 2 || (k[0] != L'/' && k[0] != L'-')) return bad(k);
        const std::wstring_view n = std::wstring_view(k).substr(1);
        if (iequals(n, L"S") || iequals(n, L"SILENT")) a.silent = true;
        else if (iequals(n, L"DESKTOP")) a.desktop = true;
        else if (iequals(n, L"NOSTARTMENU")) a.noStartMenu = true;
        else if (iequals(n, L"AUTOSTART")) a.autostart = true;
        else if (iequals(n, L"LAUNCH")) a.launch = true;
        else if (iequals(n, L"UNINSTALL")) a.uninstall = true;
        else if (iequals(n, L"REMOVEDATA")) a.removeData = true;
        else if (iequals(n, L"?") || iequals(n, L"H") || iequals(n, L"HELP")) a.help = true;
        else return bad(k);
    }
    if (a.removeData && !a.uninstall) return bad(L"/REMOVEDATA");
    if (a.uninstall && !a.dir.empty()) return bad(L"/D=");
    return a;
}

} // namespace st::setup
