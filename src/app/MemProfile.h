#pragma once
// Heap profile of our own allocations (dev builds only: `cmake -DST_MEMPROFILE=ON`, which compiles MemProfile.cpp and
// defines ST_MEMPROFILE). That file replaces the global operator new / delete; with SHADETUBE_MEMPROFILE=1 every block
// carries a 16-byte header naming the call stack that allocated it, and live bytes are tallied per stack (a fixed
// table, one lock). Without the variable it is the plain malloc-based operator new. Memory that system libraries take
// straight from a heap (Direct2D, DirectWrite, the driver, Media Foundation) is not seen: the report's heap totals
// (core/MemoryStats) show that part. Absolute numbers of a profiled run are inflated by the headers and the table;
// it is for attribution, the normal build for measurements. Symbols come from the exe's PDB (DbgHelp), so build it
// with /Zi (see docs/DEVELOPMENT.md 5.4, SHADETUBE_MEMPROFILE).
#include <string>

namespace st::app::memprofile {

#ifdef ST_MEMPROFILE
bool enabled();
// Live bytes by allocating function (the first frame in our namespace) and the largest call stacks. UI thread.
std::string report(size_t owners = 45, size_t stacks = 25);
#else
inline bool enabled() { return false; }
inline std::string report(size_t = 0, size_t = 0) { return {}; }
#endif

} // namespace st::app::memprofile
