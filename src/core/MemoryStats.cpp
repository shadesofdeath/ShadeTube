#include "core/MemoryStats.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <format>
#include <map>
#include <utility>
#include <vector>

namespace st::mem {

namespace {
double mb(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
double kb(uint64_t bytes) { return static_cast<double>(bytes) / 1024.0; }

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

// ---------------------------------------------------------------------------------------------------
// detailedReport()

namespace {

struct Range {
    uintptr_t begin = 0, end = 0;
};

bool inRanges(const std::vector<Range>& ranges, uintptr_t a) {
    // ranges sorted by begin
    auto it = std::upper_bound(ranges.begin(), ranges.end(), a, [](uintptr_t v, const Range& r) { return v < r.begin; });
    if (it == ranges.begin()) return false;
    --it;
    return a >= it->begin && a < it->end;
}

struct ThreadInfo {
    DWORD id = 0;
    std::wstring name;
    uint64_t committed = 0, reserved = 0;
    Range range;
};

// The stack of every thread of this process: NtQueryInformationThread(ThreadBasicInformation) gives its TEB, whose
// NT_TIB names the stack's top (StackBase) and the lowest committed page (StackLimit).
std::vector<ThreadInfo> threadStacks() {
    struct BasicInfo {
        LONG exitStatus;
        PVOID teb;
        HANDLE process, thread;
        ULONG_PTR affinity;
        LONG priority, basePriority;
    };
    using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    std::vector<ThreadInfo> out;
    if (!query) return out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{sizeof te};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ThreadInfo info;
        info.id = te.th32ThreadID;
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
            info.name = desc;
            LocalFree(desc);
        }
        BasicInfo bi{};
        if (query(h, 0, &bi, sizeof bi, nullptr) >= 0 && bi.teb) {
            const auto* tib = static_cast<const NT_TIB*>(bi.teb);
            const auto top = reinterpret_cast<uintptr_t>(tib->StackBase);
            MEMORY_BASIC_INFORMATION mbi{};
            if (top && VirtualQuery(tib->StackLimit, &mbi, sizeof mbi) == sizeof mbi) {
                const auto base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                info.range = {base, top};
                info.reserved = top - base;
                for (uintptr_t p = base; p < top && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) == sizeof mbi;) {
                    if (mbi.State == MEM_COMMIT) info.committed += mbi.RegionSize;
                    p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                }
            }
        }
        CloseHandle(h);
        out.push_back(std::move(info));
    }
    CloseHandle(snap);
    return out;
}

struct HeapInfo {
    HANDLE heap = nullptr;
    uint64_t committed = 0, allocated = 0;
    size_t blocks = 0;
};

} // namespace

std::string detailedReport() {
    std::string out;
    const Snapshot s = take();
    out += std::format("process: commit {:.1f} MB, private ws {:.1f} MB, ws {:.1f} MB; committed private {:.1f} MB, "
                       "mapped {:.1f} MB, image {:.1f} MB\n",
                       mb(s.privateCommit), mb(s.privateWorkingSet), mb(s.workingSet), mb(s.committedPrivate),
                       mb(s.committedMapped), mb(s.committedImage));

    // Heaps: summary of each, the regions they own (to tell heap memory from other private regions) and the default
    // heap's busy blocks by size.
    std::vector<HANDLE> heaps(256);
    const DWORD nHeaps = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
    heaps.resize(std::min<DWORD>(nHeaps, static_cast<DWORD>(heaps.size())));
    std::vector<HeapInfo> heapInfo;
    std::vector<Range> heapRanges;
    constexpr size_t kBuckets[] = {64, 256, 1024, 4096, 16384, 65536, 262144, SIZE_MAX};
    uint64_t bucketBytes[std::size(kBuckets)] = {}, bucketCount[std::size(kBuckets)] = {};
    const HANDLE defaultHeap = GetProcessHeap();
    for (HANDLE h : heaps) {
        HeapInfo hi;
        hi.heap = h;
        HEAP_SUMMARY hs{};
        hs.cb = sizeof hs;
        if (HeapSummary(h, 0, &hs)) {
            hi.committed = hs.cbCommitted;
            hi.allocated = hs.cbAllocated;
        }
        if (HeapLock(h)) {
            PROCESS_HEAP_ENTRY e{};
            while (HeapWalk(h, &e)) {
                const auto data = reinterpret_cast<uintptr_t>(e.lpData);
                if (e.wFlags & PROCESS_HEAP_REGION) {
                    heapRanges.push_back({data, reinterpret_cast<uintptr_t>(e.Region.lpLastBlock)});
                } else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                    ++hi.blocks;
                    if (e.cbData >= 256 * 1024) heapRanges.push_back({data & ~uintptr_t(0xFFFF), data + e.cbData});
                    if (h == defaultHeap) {
                        size_t b = 0;
                        while (e.cbData > kBuckets[b]) ++b;
                        bucketBytes[b] += e.cbData;
                        ++bucketCount[b];
                    }
                }
            }
            HeapUnlock(h);
        }
        heapInfo.push_back(hi);
    }
    std::sort(heapRanges.begin(), heapRanges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
    uint64_t heapsCommitted = 0;
    for (const auto& hi : heapInfo) heapsCommitted += hi.committed;

    auto threads = threadStacks();
    std::vector<Range> stackRanges;
    uint64_t stacksCommitted = 0;
    for (const auto& t : threads) {
        stacksCommitted += t.committed;
        if (t.range.end > t.range.begin) stackRanges.push_back(t.range);
    }
    std::sort(stackRanges.begin(), stackRanges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });

    // Committed private memory that is neither heap nor stack, grouped by allocation; and the mapped views.
    struct Alloc {
        uint64_t committed = 0;
        DWORD protect = 0;
    };
    std::map<uintptr_t, Alloc> other;
    struct View {
        uint64_t committed = 0;
        std::wstring file;
    };
    std::map<uintptr_t, View> views;
    uint64_t otherTotal = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi;) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const auto allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && !inRanges(heapRanges, base) &&
            !inRanges(stackRanges, base)) {
            auto& a = other[allocBase];
            a.committed += mbi.RegionSize;
            a.protect = mbi.Protect;
            otherTotal += mbi.RegionSize;
        } else if (mbi.State == MEM_COMMIT && mbi.Type == MEM_MAPPED) {
            auto& v = views[allocBase];
            if (v.committed == 0) {
                wchar_t name[MAX_PATH] = {};
                if (GetMappedFileNameW(GetCurrentProcess(), mbi.BaseAddress, name, MAX_PATH)) {
                    v.file = name;
                    if (const auto slash = v.file.find_last_of(L'\\'); slash != std::wstring::npos) v.file.erase(0, slash + 1);
                } else {
                    v.file = L"(pagefile-backed section)";
                }
            }
            v.committed += mbi.RegionSize;
        }
        uint8_t* next = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }

    out += std::format("private split: heaps {:.1f} MB ({} heaps), thread stacks {:.1f} MB ({} threads), other private "
                       "{:.1f} MB (driver / runtime VirtualAlloc; heap large blocks count as heap)\n",
                       mb(heapsCommitted), heapInfo.size(), mb(stacksCommitted), threads.size(), mb(otherTotal));

    std::sort(heapInfo.begin(), heapInfo.end(), [](const HeapInfo& a, const HeapInfo& b) { return a.committed > b.committed; });
    out += "heaps (committed / allocated, blocks):\n";
    for (const auto& hi : heapInfo) {
        if (hi.committed < 64 * 1024 && hi.heap != defaultHeap) continue;
        out += std::format("  {:>8.1f} KB / {:>8.1f} KB, {:>6} blocks{}\n", kb(hi.committed), kb(hi.allocated), hi.blocks,
                           hi.heap == defaultHeap ? "  (default process heap: CRT, most system DLLs)" : "");
    }
    out += "default heap blocks by size (count, KB):";
    static const char* kNames[] = {"<=64", "<=256", "<=1K", "<=4K", "<=16K", "<=64K", "<=256K", ">256K"};
    for (size_t b = 0; b < std::size(kBuckets); ++b)
        out += std::format(" {} {}/{:.0f}", kNames[b], bucketCount[b], kb(bucketBytes[b]));
    out += "\n";

    std::sort(threads.begin(), threads.end(), [](const ThreadInfo& a, const ThreadInfo& b) { return a.committed > b.committed; });
    out += "thread stacks (committed / reserved KB):";
    for (const auto& t : threads) {
        std::string name;
        for (wchar_t c : t.name) name += c < 128 ? static_cast<char>(c) : '?';
        out += std::format(" [{} {:.0f}/{:.0f}]", name.empty() ? std::to_string(t.id) : name, kb(t.committed), kb(t.reserved));
    }
    out += "\n";

    std::vector<std::pair<uintptr_t, Alloc>> big(other.begin(), other.end());
    std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.second.committed > b.second.committed; });
    out += "largest other private allocations (KB @ base, protection):";
    for (size_t i = 0; i < big.size() && i < 16; ++i)
        out += std::format(" {:.0f}@{:x}/{:x}", kb(big[i].second.committed), big[i].first, big[i].second.protect);
    out += std::format(" ... ({} allocations)\n", big.size());

    std::vector<std::pair<uintptr_t, View>> mapped(views.begin(), views.end());
    std::sort(mapped.begin(), mapped.end(), [](const auto& a, const auto& b) { return a.second.committed > b.second.committed; });
    out += "largest mapped views (KB name):";
    for (size_t i = 0; i < mapped.size() && i < 12; ++i) {
        std::string name;
        for (wchar_t c : mapped[i].second.file) name += c < 128 ? static_cast<char>(c) : '?';
        out += std::format(" {:.0f} {};", kb(mapped[i].second.committed), name);
    }
    out += std::format(" ({} views)\n", mapped.size());
    return out;
}

} // namespace st::mem
