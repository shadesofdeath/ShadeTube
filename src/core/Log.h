#pragma once
// Minimal thread-safe logger: %LOCALAPPDATA%\ShadeTube\logs\shadetube.log + OutputDebugString.
// Usage: ST_LOG_INFO("mb", "loaded {} albums", n);
#include <format>
#include <string>
#include <string_view>

namespace st::log {

enum class Level { Debug, Info, Warn, Error };

void init();      // opens the log file (rotates when > 2 MB)
void shutdown();
void write(Level level, std::string_view tag, std::string_view message);

template <class... Args>
void writef(Level level, std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    write(level, tag, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace st::log

#define ST_LOG_DEBUG(tag, ...) ::st::log::writef(::st::log::Level::Debug, tag, __VA_ARGS__)
#define ST_LOG_INFO(tag, ...) ::st::log::writef(::st::log::Level::Info, tag, __VA_ARGS__)
#define ST_LOG_WARN(tag, ...) ::st::log::writef(::st::log::Level::Warn, tag, __VA_ARGS__)
#define ST_LOG_ERROR(tag, ...) ::st::log::writef(::st::log::Level::Error, tag, __VA_ARGS__)
