#include "core/Log.h"

#include "core/Paths.h"
#include "core/Utf.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace st::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;

const char* levelName(Level l) {
    switch (l) {
    case Level::Debug: return "DBG";
    case Level::Info: return "INF";
    case Level::Warn: return "WRN";
    default: return "ERR";
    }
}
} // namespace

void init() {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    const auto dir = paths::logDir();
    const auto path = dir / L"shadetube.log";
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 2 * 1024 * 1024)
        std::filesystem::rename(path, dir / L"shadetube.old.log", ec);
    // Shared: an instance started while the previous one is still finishing its workers (the single-instance mutex is
    // released as soon as the windows close) must still get a log. "ab" appends each write at the end.
    g_file = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void write(Level level, std::string_view tag, std::string_view message) {
#ifdef NDEBUG
    if (level == Level::Debug) return;
#endif
    const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    const std::string line = std::format("{:%H:%M:%S} [{}] {:<8} {}\n",
                                         std::chrono::floor<std::chrono::milliseconds>(now), levelName(level), tag,
                                         message);
    OutputDebugStringW(toWide(line).c_str());
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fwrite(line.data(), 1, line.size(), g_file);
        if (level >= Level::Warn) fflush(g_file);
    }
}

} // namespace st::log
