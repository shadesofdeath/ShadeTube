#pragma once
// Crash reports: a minidump of the process when it dies of an unhandled SEH exception (access violation, stack
// overflow...), std::terminate, a pure virtual call or an invalid CRT parameter.
//
// Dumps go to %LOCALAPPDATA%\ShadeTube\crashes\ShadeTube-<yyyymmdd-hhmmss>-<pid>.dmp (thread stacks + the memory they
// reference, a few MB; the newest kMaxDumps are kept). They are written by a dedicated thread created up front, so
// a crash on a thread with no stack left (stack overflow) or holding a lock can still be reported. Nothing is ever
// uploaded: the user can open the folder from Ayarlar and send a dump themselves.
#include <filesystem>
#include <optional>

namespace st::crash {

constexpr int kMaxDumps = 5;

// Installs the handlers. Call once, first thing in wWinMain (paths::appData() must be usable).
void install();

std::filesystem::path dumpDir();
int dumpCount();

// The dump of an earlier run the user has not been told about yet; clears the "unreported" marker.
std::optional<std::filesystem::path> takeUnreportedCrash();

// Dev (--crash-test): crash on purpose to check the handler end to end.
[[noreturn]] void crashForTest();

} // namespace st::crash
