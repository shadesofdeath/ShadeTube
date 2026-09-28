#include "core/I18n.h"

#include "core/Log.h"
#include "core/Resources.h"
#include "core/Utf.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace st {
void setCaseLocale(const wchar_t* localeName);   // core/Utf.cpp (toUpperTr / toLowerTr)
}

namespace st::i18n {

namespace {

constexpr Language kLanguages[] = {
    {"tr", L"Türkçe", L"tr-TR"},
    {"en", L"English", L"en-US"},
    {"de", L"Deutsch", L"de-DE"},
    {"es", L"Español", L"es-ES"},
    {"fr", L"Français", L"fr-FR"},
    {"pt", L"Português (Brasil)", L"pt-BR"},
    {"ru", L"Русский", L"ru-RU"},
    {"uk", L"Українська", L"uk-UA"},
    {"id", L"Bahasa Indonesia", L"id-ID"},
    {"ja", L"日本語", L"ja-JP"},
    {"ko", L"한국어", L"ko-KR"},
};

// CLDR plural categories used by the shipped languages (integers only).
enum Cat { One, Few, Many, Other, kCats };

Cat category(std::string_view code, long long n) {
    const long long a = n < 0 ? -n : n;
    if (code == "ru" || code == "uk") {
        const long long m10 = a % 10, m100 = a % 100;
        if (m10 == 1 && m100 != 11) return One;
        if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return Few;
        return Many;
    }
    if (code == "fr" || code == "pt") return a <= 1 ? One : Other;
    if (code == "ja" || code == "ko" || code == "id" || code == "tr") return Other;
    return a == 1 ? One : Other;   // en, de, es
}

struct Hash {
    using is_transparent = void;
    size_t operator()(std::wstring_view s) const noexcept { return std::hash<std::wstring_view>{}(s); }
};

struct Entry {
    std::array<std::wstring, kCats> forms;   // missing forms are filled with "other"
};

const Language* g_lang = &kLanguages[0];
// Built once by init() before any window exists, never modified afterwards: lookups need no lock.
std::unordered_map<std::wstring, Entry, Hash, std::equal_to<>> g_table;

std::mutex g_missingMutex;
std::unordered_set<std::wstring, Hash, std::equal_to<>> g_missing;

const Entry* find(std::wstring_view key) {
    if (isSource()) return nullptr;
    if (auto it = g_table.find(key); it != g_table.end()) return &it->second;
    std::lock_guard lock(g_missingMutex);
    if (g_missing.emplace(key).second) ST_LOG_WARN("i18n", "missing {} text: \"{}\"", g_lang->code, toUtf8(key));
    return nullptr;
}

const Language* byCode(std::string_view code) {
    for (const auto& l : kLanguages)
        if (code == l.code) return &l;
    return nullptr;
}

// Windows' display language ("pt-PT" -> "pt"), else English for everyone else.
const Language* fromWindows() {
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 0) {
        std::wstring buf(size, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) {
            for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
                std::string code;
                for (const wchar_t* c = p; *c && *c != L'-'; ++c) code += static_cast<char>(std::tolower(static_cast<int>(*c & 0x7F)));
                if (const Language* l = byCode(code)) return l;
            }
        }
    }
    return byCode("en");
}

std::wstring localeString(LCTYPE type) {
    wchar_t buf[128];
    const int n = GetLocaleInfoEx(localeName(), type, buf, 128);
    return n > 1 ? std::wstring(buf, static_cast<size_t>(n - 1)) : std::wstring();
}

} // namespace

std::span<const Language> languages() { return kLanguages; }
const Language& current() { return *g_lang; }
bool isSource() { return g_lang == &kLanguages[0]; }

const Language& resolve(std::string_view code) {
    const Language* l = code.empty() ? fromWindows() : byCode(code);
    return l ? *l : kLanguages[0];
}

void init(std::string_view code) {
    g_lang = &resolve(code);
    setCaseLocale(g_lang->locale);
    if (isSource()) return;
    const std::wstring file = toWide(g_lang->code) + L".json";
    // Dev / translators: SHADETUBE_I18N_DIR=<folder with <code>.json> is read instead of the embedded copy, so a
    // translation can be checked in the app without rebuilding.
    std::string disk;
    wchar_t dir[MAX_PATH];
    if (const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_I18N_DIR", dir, MAX_PATH); n > 0 && n < MAX_PATH) {
        if (FILE* f = _wfopen((std::wstring(dir) + L"\\" + file).c_str(), L"rb")) {
            char buf[65536];
            for (size_t got; (got = fread(buf, 1, sizeof buf, f)) > 0;) disk.append(buf, got);
            fclose(f);
            ST_LOG_INFO("i18n", "{} read from SHADETUBE_I18N_DIR", toUtf8(file));
        }
    }
    const std::string_view raw = disk.empty() ? res::load(L"i18n/" + file) : std::string_view(disk);
    const auto j = nlohmann::json::parse(raw.begin(), raw.end(), nullptr, false);
    if (!j.is_object()) {
        ST_LOG_ERROR("i18n", "i18n/{}.json missing or invalid: the UI stays Turkish", g_lang->code);
        return;
    }
    static constexpr const char* kCatNames[kCats] = {"one", "few", "many", "other"};
    for (const auto& [key, value] : j.items()) {
        Entry e;
        if (value.is_string()) {
            e.forms[Other] = toWide(value.get<std::string>());
        } else if (value.is_object()) {
            for (int c = 0; c < kCats; ++c)
                if (auto it = value.find(kCatNames[c]); it != value.end() && it->is_string())
                    e.forms[c] = toWide(it->get<std::string>());
        }
        // "other" is the fallback form; a file that only gives ru/uk's one/few/many still works.
        if (e.forms[Other].empty()) e.forms[Other] = !e.forms[Many].empty() ? e.forms[Many] : !e.forms[Few].empty() ? e.forms[Few] : e.forms[One];
        if (e.forms[Other].empty()) continue;
        for (auto& f : e.forms)
            if (f.empty()) f = e.forms[Other];
        g_table.emplace(toWide(key), std::move(e));
    }
    ST_LOG_INFO("i18n", "UI language {} ({} texts)", g_lang->code, g_table.size());
}

const wchar_t* tr(const wchar_t* turkish) {
    const Entry* e = find(turkish);
    return e ? e->forms[Other].c_str() : turkish;
}

std::wstring tr(const std::wstring& turkish) {
    const Entry* e = find(turkish);
    return e ? e->forms[Other] : turkish;
}

std::wstring format(std::wstring_view pattern, std::initializer_list<std::wstring_view> args) {
    std::wstring out;
    out.reserve(pattern.size() + 16);
    auto arg = args.begin();
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == L'{' && i + 1 < pattern.size() && pattern[i + 1] == L'}' && arg != args.end()) {
            out += *arg++;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

std::wstring format(std::wstring_view pattern, long long n) {
    const std::wstring s = std::to_wstring(n);
    return format(pattern, {s});
}

static std::wstring_view pluralForm(const wchar_t* turkish, long long n) {
    const Entry* e = find(turkish);
    return e ? std::wstring_view(e->forms[category(g_lang->code, n)]) : std::wstring_view(turkish);
}

std::wstring plural(const wchar_t* turkish, long long n) {
    const std::wstring shown = number(n);
    return format(pluralForm(turkish, n), {shown});
}

std::wstring plural(const wchar_t* turkish, long long n, std::initializer_list<std::wstring_view> args) {
    return format(pluralForm(turkish, n), args);
}

std::wstring monthAbbrev(int month) {
    if (month < 1 || month > 12) return {};
    std::wstring s = localeString(LOCALE_SABBREVMONTHNAME1 + static_cast<LCTYPE>(month - 1));
    while (!s.empty() && s.back() == L'.') s.pop_back();   // "sept." -> "sept"
    return s;
}

std::wstring monthName(int month) {
    if (month < 1 || month > 12) return {};
    return localeString(LOCALE_SMONTHNAME1 + static_cast<LCTYPE>(month - 1));
}

std::wstring weekdayName(int weekday) {
    if (weekday < 0 || weekday > 6) return {};
    // LOCALE_SDAYNAME1 is Monday ... LOCALE_SDAYNAME7 Sunday.
    return localeString(LOCALE_SDAYNAME1 + static_cast<LCTYPE>(weekday == 0 ? 6 : weekday - 1));
}

std::wstring number(int64_t n) {
    std::wstring sep = localeString(LOCALE_STHOUSAND);
    if (sep.empty()) sep = L".";
    const std::wstring digits = std::to_wstring(n < 0 ? -n : n);
    std::wstring out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += sep;
        out += digits[i];
    }
    return n < 0 ? L"-" + out : out;
}

wchar_t decimalSeparator() {
    const std::wstring s = localeString(LOCALE_SDECIMAL);
    return s.empty() ? L',' : s[0];
}

} // namespace st::i18n
