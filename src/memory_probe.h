#pragma once
// Memory checks on the game's data that do not stall the game thread (v0.9.335).
//
// VirtualQuery sizes the whole run of identical pages around the address it is asked about. In the game's heap regions
// (hundreds of megabytes each) that walk takes milliseconds per call, and longer while the game streams: the v0.9.334
// log had the Chapter Select list at 15-40 ms a tick through a level load, Acro's 2-second check of Lara's ground graph
// at 7 ms, and the respawn setup of Brawl / Grapple / Acro / cloth at 20-100 ms, all VirtualQuery. So:
//  - readable: a guarded read of one byte per page (an access violation caught: not readable). A guard page tripped by
//    the read (another thread's stack end) gets its guard back, so that stack still grows;
//  - writable: VirtualQuery, but its answer for the game's heap (private memory) is kept for a second, so the bursts of
//    checks a respawn or a patch pass makes ask once per region. The game's image is asked every time (patches change
//    its protection).
#include <cstddef>

namespace MemoryProbe {
// Every byte of [address, address + bytes) readable now. Ranges over 1 MB go through VirtualQuery (reading a byte of
// every page of a large range would fault it all in).
bool Readable(const void* address, std::size_t bytes);
// A C string copied (at most capacity - 1 characters, always terminated) as far as it is readable; false when nothing
// was copied.
bool CopyCString(const char* source, char* destination, std::size_t capacity);
// Every byte of [address, address + bytes) committed and writable (read-write or write-copy).
bool Writable(const void* address, std::size_t bytes);
// Writable, else made read-write (VirtualProtect). The game's data the patches change.
bool MakeWritable(void* address, std::size_t bytes);
// Forgets the kept answers (the game gives memory back: a level load starts).
void Forget();
}  // namespace MemoryProbe
