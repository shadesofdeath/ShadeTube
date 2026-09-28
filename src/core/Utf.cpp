#include "core/Utf.h"

#include <windows.h>

namespace st {

std::wstring toWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Casing follows the UI language (i18n::init): tr-TR maps i -> İ, en-US i -> I. Set once at startup.
static const wchar_t* g_caseLocale = L"tr-TR";
void setCaseLocale(const wchar_t* localeName) { g_caseLocale = localeName; }

static std::wstring mapCase(std::wstring_view s, DWORD flags) {
    if (s.empty()) return {};
    const int n = LCMapStringEx(g_caseLocale, flags | LCMAP_LINGUISTIC_CASING, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    LCMapStringEx(g_caseLocale, flags | LCMAP_LINGUISTIC_CASING, s.data(), static_cast<int>(s.size()), out.data(), n,
                  nullptr, nullptr, 0);
    return out;
}

std::wstring toUpperTr(std::wstring_view s) { return mapCase(s, LCMAP_UPPERCASE); }
std::wstring toLowerTr(std::wstring_view s) { return mapCase(s, LCMAP_LOWERCASE); }

std::wstring foldForSearch(std::wstring_view s) {
    std::wstring lower = mapCase(s, LCMAP_LOWERCASE);
    // Decompose, then drop combining marks (ü -> u, ş -> s). ı has no decomposition: map explicitly.
    const int n = NormalizeString(NormalizationKD, lower.c_str(), static_cast<int>(lower.size()), nullptr, 0);
    if (n <= 0) return lower;
    std::wstring decomposed(static_cast<size_t>(n), L'\0');
    const int m = NormalizeString(NormalizationKD, lower.c_str(), static_cast<int>(lower.size()), decomposed.data(), n);
    if (m <= 0) return lower;
    decomposed.resize(static_cast<size_t>(m));
    std::wstring out;
    out.reserve(decomposed.size());
    for (wchar_t c : decomposed) {
        if (c >= 0x0300 && c <= 0x036F) continue;
        out.push_back(c == L'ı' ? L'i' : c);
    }
    return out;
}

} // namespace st
