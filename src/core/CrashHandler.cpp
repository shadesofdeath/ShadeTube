#include "core/CrashHandler.h"

#include "core/Paths.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

namespace st::crash {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kMarker[] = L"unreported.txt";   // name of the newest dump nobody was told about yet
// Our own exception codes for fatal errors that are not SEH exceptions (so the dump still has a faulting context).
constexpr DWORD kTerminate = 0xE0535401;
constexpr DWORD kPureCall = 0xE0535402;
constexpr DWORD kInvalidParameter = 0xE0535403;
constexpr DWORD kAbort = 0xE0535404;

// Everything the crash path touches is set up by install(): no allocation, no locks, no logging while crashing.
struct State {
    HANDLE request = nullptr;          // a crashing thread asks for the dump
    HANDLE done = nullptr;             // manual reset: the dump is on disk (or failed)
    EXCEPTION_POINTERS* exception = nullptr;
    DWORD threadId = 0;
    volatile LONG claimed = 0;         // the first crashing thread reports; later ones just wait for it
    wchar_t dir[MAX_PATH]{};
};
State g;

void writeDump() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[80];
    swprintf(name, 80, L"ShadeTube-%04u%02u%02u-%02u%02u%02u-%lu.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, GetCurrentProcessId());
    wchar_t path[MAX_PATH + 96];
    swprintf(path, MAX_PATH + 96, L"%s\\%s", g.dir, name);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = g.threadId;
    mei.ExceptionPointers = g.exception;
    mei.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                 MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                                      g.exception ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(path);
        return;
    }
    swprintf(path, MAX_PATH + 96, L"%s\\%s", g.dir, kMarker);
    HANDLE m = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(m, name, static_cast<DWORD>(wcslen(name) * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(m);
    }
}

DWORD WINAPI dumpThread(void*) {
    WaitForSingleObject(g.request, INFINITE);
    writeDump();
    SetEvent(g.done);
    return 0;
}

// On the crashing thread: hand the work to the dump thread (it has a healthy stack) and wait, bounded.
void requestDump(EXCEPTION_POINTERS* ep) {
    if (InterlockedCompareExchange(&g.claimed, 1, 0) == 0) {
        g.exception = ep;
        g.threadId = GetCurrentThreadId();
        SetEvent(g.request);
    }
    WaitForSingleObject(g.done, 30000);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    requestDump(ep);
    TerminateProcess(GetCurrentProcess(), ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Fatal CRT errors have no exception record: raise one of our codes so the dump shows where it happened.
void fatal(DWORD code) {
    __try {
        RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (requestDump(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
    TerminateProcess(GetCurrentProcess(), code);
}

void onTerminate() {
    fatal(kTerminate);
    std::abort();
}
void __cdecl onPureCall() { fatal(kPureCall); }
void __cdecl onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    fatal(kInvalidParameter);
}
void __cdecl onAbort(int) { fatal(kAbort); }

std::vector<fs::path> dumps() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dumpDir(), ec))
        if (e.is_regular_file(ec) && e.path().extension() == L".dmp") out.push_back(e.path());
    return out;
}

void pruneOldDumps() {
    auto all = dumps();
    if (static_cast<int>(all.size()) <= kMaxDumps) return;
    std::error_code ec;
    std::sort(all.begin(), all.end(), [&](const fs::path& a, const fs::path& b) {
        return fs::last_write_time(a, ec) > fs::last_write_time(b, ec);   // newest first
    });
    for (size_t i = kMaxDumps; i < all.size(); ++i) fs::remove(all[i], ec);
}

} // namespace

fs::path dumpDir() {
    fs::path dir = paths::appData() / L"crashes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

int dumpCount() { return static_cast<int>(dumps().size()); }

void install() {
    const std::wstring dir = dumpDir().wstring();
    if (dir.size() >= MAX_PATH) return;
    wcsncpy_s(g.dir, dir.c_str(), _TRUNCATE);
    pruneOldDumps();
    g.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g.request || !g.done) return;
    HANDLE thread = CreateThread(nullptr, 256 * 1024, dumpThread, nullptr, 0, nullptr);
    if (!thread) return;
    CloseHandle(thread);
    // Room for the filter on the main (UI) thread after a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(onTerminate);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // abort() -> SIGABRT -> our dump, no WER dialog
    std::signal(SIGABRT, onAbort);
}

std::optional<fs::path> takeUnreportedCrash() {
    const fs::path marker = dumpDir() / kMarker;
    HANDLE m = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (m == INVALID_HANDLE_VALUE) return std::nullopt;
    wchar_t name[128]{};
    DWORD read = 0;
    ReadFile(m, name, sizeof name - sizeof(wchar_t), &read, nullptr);
    CloseHandle(m);
    std::error_code ec;
    fs::remove(marker, ec);
    const fs::path dump = dumpDir() / name;
    if (read == 0 || !fs::exists(dump, ec)) return std::nullopt;
    return dump;
}

void crashForTest() {
    volatile int* p = nullptr;
    *p = 42;   // access violation -> unhandledFilter
    std::abort();
}

} // namespace st::crash
