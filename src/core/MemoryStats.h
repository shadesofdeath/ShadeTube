#pragma once
// Where the process's memory is (diagnostics): commit / working set from the OS, a walk of the address space by region
// type, and the default heap. Cheap enough to run every few seconds (a VirtualQuery walk, no heap walk). Any thread.
#include <cstdint>
#include <string>

namespace st::mem {

struct Snapshot {
    uint64_t privateCommit = 0;   // PrivateUsage: what Task Manager's "commit size" shows
    uint64_t workingSet = 0;      // resident pages (private + shared)
    uint64_t privateWorkingSet = 0;   // resident private pages: Task Manager's "Memory" column
    uint64_t committedPrivate = 0;    // committed MEM_PRIVATE regions (heaps, stacks, driver allocations)
    uint64_t committedMapped = 0;     // committed MEM_MAPPED views (file mappings, shared sections)
    uint64_t committedImage = 0;      // committed MEM_IMAGE (exe and DLLs)
    uint64_t heapCommitted = 0;       // the default process heap (CRT allocations)
    uint64_t heapAllocated = 0;
};

Snapshot take();
// One log line: "commit 64.1 MB, private ws 38.2 MB, ws 71.0 MB | private 58.9 MB (heap 12.3/9.8 MB), mapped 4.1 MB,
// image 31.7 MB".
std::string describe(const Snapshot& s);

} // namespace st::mem
