#pragma once
// UI language: Türkçe (the source language) + 10 translations.
//
// The Turkish source text is the key: tr(L"Ayarlar") returns the text from assets/i18n/<code>.json for the UI
// language, or the key itself (Turkish, or a text the file lacks — logged once). The language is fixed at startup
// (Settings::language; "" = follow Windows' display language when it is supported, else English), so lookups are
// lock-free and returned pointers live for the whole process. A change in Ayarlar applies after a restart.
//
// Text with numbers uses "{}" placeholders instead of concatenation, so word order can differ per language:
//   format(tr(L"{} şarkı"), {count})   -> "12 şarkı" / "12 songs" / "12 Titel"
//   plural(L"{} şarkı", n)             -> the translation may give CLDR plural forms:
//                                         {"one": "{} song", "other": "{} songs"}  (ru/uk also "few", "many")
// Month / weekday names and digit grouping come from Windows for the UI locale (monthAbbrev(), number()).
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace st::i18n {

struct Language {
    const char* code;          // settings value and assets/i18n/<code>.json ("tr" has no file)
    const wchar_t* name;       // native name for the picker ("Deutsch")
    const wchar_t* locale;     // Windows locale for casing, dates, numbers and Accept-Language ("de-DE")
};

std::span<const Language> languages();   // Türkçe first, then the translations
// The language a Settings value means: a known code, or for "" Windows' display language (English when it isn't
// one of ours). Unknown codes mean Türkçe.
const Language& resolve(std::string_view code);
void init(std::string_view code);        // main.cpp, right after Settings::load()
const Language& current();
inline const char* code() { return current().code; }
inline const wchar_t* localeName() { return current().locale; }
bool isSource();                         // Turkish: tr() is the identity

const wchar_t* tr(const wchar_t* turkish);
std::wstring tr(const std::wstring& turkish);

// Replaces each "{}" in `pattern` with the next argument, in order.
std::wstring format(std::wstring_view pattern, std::initializer_list<std::wstring_view> args);
std::wstring format(std::wstring_view pattern, long long n);
// tr() + format() with the plural form of the UI language for `n`, shown with the UI language's digit grouping
// ("1.234 şarkı" / "1,234 songs").
std::wstring plural(const wchar_t* turkish, long long n);
// The same form choice for `n`, but the "{}"s are filled from `args` in order: a count shown another way
// (plural(L"{} DİNLENME", n, {compactCount(n)}) -> "12,4 B DİNLENME") or more placeholders after it.
std::wstring plural(const wchar_t* turkish, long long n, std::initializer_list<std::wstring_view> args);

// Locale data (from Windows for localeName()).
std::wstring monthAbbrev(int month);     // 1..12: "Eyl" / "Sep" / "сент."
std::wstring monthName(int month);       // 1..12: "Eylül" / "September"
std::wstring weekdayName(int weekday);   // 0 = Sunday .. 6: "Pazartesi" / "Monday"
std::wstring number(int64_t n);          // digit grouping: "1.234.567" / "1,234,567"
wchar_t decimalSeparator();              // ',' / '.'

} // namespace st::i18n

namespace st {
using i18n::tr;
}
