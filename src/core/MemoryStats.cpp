#include "core/MemoryStats.h"

#include <windows.h>
#include <psapi.h>

#include <format>

namespace st::mem {

namespace {
double mb(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 21H2+), declared here because the SDK hides it behind a newer NTDDI_VERSION.
struct CountersEx2 {
    PROCESS_MEMORY_COUNTERS_EX ex;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
} // namespace

Snapshot take() {
    Snapshot s;
    CountersEx2 pmc{};
    pmc.ex.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        s.privateCommit = pmc.ex.PrivateUsage;
        s.workingSet = pmc.ex.WorkingSetSize;
        s.privateWorkingSet = pmc.PrivateWorkingSetSize;
    } else {
        PROCESS_MEMORY_COUNTERS_EX old{};
        old.cb = sizeof old;
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&old), sizeof old)) {
            s.privateCommit = old.PrivateUsage;
            s.workingSet = old.WorkingSetSize;
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi;) {
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Type == MEM_PRIVATE) s.committedPrivate += mbi.RegionSize;
            else if (mbi.Type == MEM_MAPPED) s.committedMapped += mbi.RegionSize;
            else if (mbi.Type == MEM_IMAGE) s.committedImage += mbi.RegionSize;
        }
        uint8_t* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    HEAP_SUMMARY hs{};
    hs.cb = sizeof hs;
    if (HeapSummary(GetProcessHeap(), 0, &hs)) {
        s.heapCommitted = hs.cbCommitted;
        s.heapAllocated = hs.cbAllocated;
    }
    return s;
}

std::string describe(const Snapshot& s) {
    return std::format("commit {:.1f} MB, private ws {:.1f} MB, ws {:.1f} MB | private {:.1f} MB (heap {:.1f}/{:.1f} MB), "
                       "mapped {:.1f} MB, image {:.1f} MB",
                       mb(s.privateCommit), mb(s.privateWorkingSet), mb(s.workingSet), mb(s.committedPrivate),
                       mb(s.heapAllocated), mb(s.heapCommitted), mb(s.committedMapped), mb(s.committedImage));
}

} // namespace st::mem
