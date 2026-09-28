#include "core/Resources.h"

#include <windows.h>

#include <string>

namespace st::res {

std::string_view load(std::wstring_view name) {
    // resources.rc names are quoted strings ("icons/24/play.svg"); rc.exe keeps the quotes in the name.
    std::wstring key = L"\"";
    key += name;
    key += L'"';
    for (auto& c : key)
        if (c == L'\\') c = L'/';
    HRSRC info = FindResourceW(nullptr, key.c_str(), RT_RCDATA);
    if (!info) return {};
    HGLOBAL handle = LoadResource(nullptr, info);
    if (!handle) return {};
    const void* data = LockResource(handle);
    return {static_cast<const char*>(data), SizeofResource(nullptr, info)};
}

bool exists(std::wstring_view name) { return !load(name).empty(); }

} // namespace st::res
