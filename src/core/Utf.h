#pragma once
// UTF-8 <-> UTF-16 conversion. All model strings are UTF-8; convert only at Win32/DirectWrite edges.
#include <string>
#include <string_view>

namespace st {

std::wstring toWide(std::string_view utf8);
std::string toUtf8(std::wstring_view wide);

// Locale-aware (tr-TR) case mapping: i -> İ, ı -> I. Used for mono uppercase labels.
std::wstring toUpperTr(std::wstring_view s);
std::wstring toLowerTr(std::wstring_view s);

// Accent/case-insensitive fold used for local filtering ("sürüş" matches "SURUS").
std::wstring foldForSearch(std::wstring_view s);

} // namespace st
