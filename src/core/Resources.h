#pragma once
// Access to files embedded in the executable (assets/ is compiled into resources.rc as RCDATA,
// named by their path relative to assets/, e.g. "icons/24/play.svg"). Zero-copy: the returned view
// points into the mapped image and lives for the whole process.
#include <string_view>

namespace st::res {

std::string_view load(std::wstring_view name);   // empty view if missing
bool exists(std::wstring_view name);

} // namespace st::res
