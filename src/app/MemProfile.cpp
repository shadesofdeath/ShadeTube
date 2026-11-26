// Heap profile of our own allocations (ST_MEMPROFILE builds; see MemProfile.h).
#include "app/MemProfile.h"

#include <windows.h>
#include <dbghelp.h>
#include <new.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace {

constexpr int kDepth = 14;
constexpr size_t kSlots = size_t(1) << 17;
constexpr uint32_t kMagic = 0x5348ADE5;

struct Slot {
    uint32_t hash = 0;
    uint16_t depth = 0;
    uint16_t used = 0;
    void* frames[kDepth] = {};
    int64_t liveBytes = 0, liveBlocks = 0, totalBlocks = 0;
};
struct Header {   // 16 bytes: the block keeps malloc's 16-byte alignment
    uint64_t size;
    uint32_t slot;
    uint32_t magic;
};

Slot* g_slots = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<int> g_mode{0};   // 0 = not decided yet, 1 = off, 2 = on (decided by the very first allocation)

int mode() {
    int m = g_mode.load(std::memory_order_acquire);
    if (m) return m;
    wchar_t v[4] = {};
    bool on = GetEnvironmentVariableW(L"SHADETUBE_MEMPROFILE", v, 4) > 0 && v[0] != L'0';
    if (on) {
        g_slots = static_cast<Slot*>(VirtualAlloc(nullptr, sizeof(Slot) * kSlots, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        on = g_slots != nullptr;
    }
    int expected = 0;
    g_mode.compare_exchange_strong(expected, on ? 2 : 1);
    return g_mode.load();
}

uint32_t slotFor(void** frames, uint16_t depth) {
    uint64_t h = 1469598103934665603ULL;
    for (uint16_t i = 0; i < depth; ++i) {
        h ^= reinterpret_cast<uintptr_t>(frames[i]);
        h *= 1099511628211ULL;
    }
    const uint32_t hash = static_cast<uint32_t>(h ^ (h >> 32)) | 1u;
    for (size_t probe = 0; probe < kSlots; ++probe) {
        const size_t i = (hash + probe) & (kSlots - 1);
        Slot& s = g_slots[i];
        if (!s.used) {
            s.used = 1;
            s.hash = hash;
            s.depth = depth;
            std::copy(frames, frames + depth, s.frames);
            return static_cast<uint32_t>(i);
        }
        if (s.hash == hash && s.depth == depth && std::equal(frames, frames + depth, s.frames)) return static_cast<uint32_t>(i);
    }
    return 0;   // full: slot 0 collects the rest
}

void* profiledAlloc(size_t n) {
    auto* h = static_cast<Header*>(malloc(n + sizeof(Header)));
    if (!h) return nullptr;
    void* frames[kDepth];
    const USHORT depth = RtlCaptureStackBackTrace(2, kDepth, frames, nullptr);
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t slot = slotFor(frames, depth);
    Slot& s = g_slots[slot];
    s.liveBytes += static_cast<int64_t>(n);
    ++s.liveBlocks;
    ++s.totalBlocks;
    ReleaseSRWLockExclusive(&g_lock);
    h->size = n;
    h->slot = slot;
    h->magic = kMagic;
    return h + 1;
}

void profiledFree(void* p) {
    auto* h = static_cast<Header*>(p) - 1;
    if (h->magic != kMagic) {   // never ours (cannot happen: the mode is fixed by the first allocation)
        free(p);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Slot& s = g_slots[h->slot];
    s.liveBytes -= static_cast<int64_t>(h->size);
    --s.liveBlocks;
    ReleaseSRWLockExclusive(&g_lock);
    h->magic = 0;
    free(h);
}

void* allocate(size_t n) {
    if (n == 0) n = 1;
    for (;;) {
        void* p = mode() == 2 ? profiledAlloc(n) : malloc(n);
        if (p) return p;
        if (_callnewh(n) == 0) return nullptr;
    }
}

void release(void* p) noexcept {
    if (!p) return;
    if (mode() == 2) profiledFree(p);
    else free(p);
}

} // namespace

void* operator new(size_t n) {
    if (void* p = allocate(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    if (void* p = allocate(n)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

namespace st::app::memprofile {

bool enabled() { return mode() == 2; }

std::string report(size_t owners, size_t stacks) {
    if (!enabled()) return {};
    // Copy the live slots first: anything allocating while the lock is held would deadlock on it.
    struct Live {
        int64_t bytes, blocks, total;
        uint32_t slot;
    };
    std::vector<Live> live;
    live.reserve(kSlots);
    int64_t trackedBytes = 0, trackedBlocks = 0;
    AcquireSRWLockShared(&g_lock);
    for (uint32_t i = 0; i < kSlots; ++i) {
        const Slot& s = g_slots[i];
        if (!s.used || s.liveBlocks <= 0) continue;
        live.push_back({s.liveBytes, s.liveBlocks, s.totalBlocks, i});
        trackedBytes += s.liveBytes;
        trackedBlocks += s.liveBlocks;
    }
    ReleaseSRWLockShared(&g_lock);
    std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return a.bytes > b.bytes; });

    static bool symbols = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    std::unordered_map<void*, std::string> names;
    auto nameOf = [&](void* addr) -> const std::string& {
        auto [it, inserted] = names.try_emplace(addr);
        if (!inserted) return it->second;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (symbols && SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &disp, sym)) it->second = sym->Name;
        else it->second = std::format("{}", addr);
        return it->second;
    };
    // The owner of a stack: its first frame in our code that is not an allocation helper.
    auto ownerOf = [&](const Slot& s) -> std::string {
        for (uint16_t i = 0; i < s.depth; ++i) {
            const std::string& n = nameOf(s.frames[i]);
            if (n.starts_with("st::") || n.starts_with("YoutubeExplode::") || n.starts_with("wWinMain")) return n;
        }
        return s.depth ? "(system) " + nameOf(s.frames[std::min<uint16_t>(s.depth - 1, 3)]) : "(no stack)";
    };

    std::unordered_map<std::string, Live> byOwner;
    for (const auto& l : live) {
        auto& o = byOwner[ownerOf(g_slots[l.slot])];
        o.bytes += l.bytes;
        o.blocks += l.blocks;
        o.total += l.total;
    }
    std::vector<std::pair<std::string, Live>> ownerList(byOwner.begin(), byOwner.end());
    std::sort(ownerList.begin(), ownerList.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });

    std::string out = std::format("operator new, live: {:.2f} MB in {} blocks from {} call stacks (profiler: +{} B/block, "
                                  "{:.1f} MB table)\n",
                                  trackedBytes / 1048576.0, trackedBlocks, live.size(), sizeof(Header),
                                  sizeof(Slot) * kSlots / 1048576.0);
    out += "by owner (KB, blocks):\n";
    for (size_t i = 0; i < ownerList.size() && i < owners; ++i)
        out += std::format("  {:>9.1f} {:>7}  {}\n", ownerList[i].second.bytes / 1024.0, ownerList[i].second.blocks,
                           ownerList[i].first);
    out += "largest stacks:\n";
    for (size_t i = 0; i < live.size() && i < stacks; ++i) {
        const Slot& s = g_slots[live[i].slot];
        out += std::format("  {:.1f} KB in {} blocks:", live[i].bytes / 1024.0, live[i].blocks);
        int shown = 0;
        for (uint16_t f = 0; f < s.depth && shown < 7; ++f) {
            const std::string& n = nameOf(s.frames[f]);
            if (n.starts_with("std::") || n.find("operator new") != std::string::npos || n.starts_with("malloc")) continue;
            out += (shown++ ? " < " : " ") + n;
        }
        out += "\n";
    }
    return out;
}

} // namespace st::app::memprofile
