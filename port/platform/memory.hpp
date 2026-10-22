#pragma once

#include <cstddef>
#include <cstdint>

// The arenas' memory: ordinary anonymous mappings wherever the system puts them. The game's casts of
// pointers to 32-bit integers are all widened in port/src (docs/port/truncations.md), so nothing
// needs the low 4 GiB, which arm64 macOS cannot map at all.

struct ArenaMemory {
    unsigned char *map = nullptr;
    std::size_t    map_size = 0; // guard page included
    unsigned char *base = nullptr;
    std::size_t    capacity = 0;
    std::size_t    lead = 0;
};

// Zeroed: capacity (bytes rounded up to 64) usable bytes at base, lead usable bytes before base,
// then an inaccessible page. Aborts with a message when the memory cannot be had.
ArenaMemory ArenaMemoryMap(std::size_t bytes, std::size_t lead = 0);
// Zeroes every usable byte (lead included) and returns the pages to the system.
void ArenaMemoryZero(const ArenaMemory &memory);
// macOS's zeroing (a fixed anonymous mapping over the range), callable anywhere for tests.
void ArenaMemoryZeroByRemap(const ArenaMemory &memory);
// The first byte of the inaccessible page after base + capacity.
const unsigned char *ArenaMemoryGuard(const ArenaMemory &memory);
void                 ArenaMemoryUnmap(ArenaMemory &memory);
std::size_t          ArenaMemoryPageSize();

// The arenas' shared reservation, at a fixed address: every launch tries the same candidates in
// the same order, so the game's pointers into the reservation survive a restart and a state can be
// put back into it. `map` is nullptr when no candidate was free. The reservation is virtual only
// (MAP_NORESERVE, MEM_RESERVE); a block's pages are committed as its blocks are carved.
struct ArenaRegionReservation {
    unsigned char *map = nullptr;
    std::size_t    size = 0;
};
ArenaRegionReservation ArenaRegionReserve(std::size_t bytes);
// Makes a sub-range of the reservation usable (a block, on the platforms that reserve without
// committing), takes its pages back (a retired block), and closes a block's guard page.
void ArenaRegionCommit(void *at, std::size_t bytes);
void ArenaRegionDecommit(void *at, std::size_t bytes);
void ArenaRegionProtect(void *at, std::size_t bytes);

inline bool IsAbove4GiB(const void *pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer) >> 32 != 0;
}
