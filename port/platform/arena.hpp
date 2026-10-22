#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/memory.hpp"

// Every arena the port carves gets this many times the quadwords retail asked
// for: the game sizes many allocations with the host's sizeof, which grows
// with 8-byte pointers.
constexpr int kArenaHeadroom = 4;

// A zeroed, 64-byte aligned block of at least `bytes`, with `lead` usable
// bytes before it, kept for `owner` and handed back to it while it fits. An
// unmapped page follows it so a run past the end faults at once. A debug build
// stops if no block lies above 4 GiB, so a mapping that drifted low cannot hide
// a pointer the game truncates.
unsigned char *ArenaBlock(const void *owner, std::size_t bytes, std::size_t lead = 0);

// Zeroes every block, as retail's clear of the whole GlobalDataBuffer does.
void ArenaClearAll();

// What a live block is, for saving the game's state: the owner that holds it, its whole mapping,
// and the usable window inside it (lead bytes before base, capacity after).
struct ArenaBlockInfo {
    const void    *owner = nullptr;
    unsigned char *map = nullptr;
    std::size_t    map_size = 0;
    unsigned char *base = nullptr;
    std::size_t    capacity = 0;
    std::size_t    lead = 0;
};

// The live blocks, in mapping order.
std::size_t   ArenaBlockCount();
ArenaBlockInfo ArenaBlockInfoAt(std::size_t index);

// Puts one block back where a saved state recorded it: mapped at `map` again, with the recorded
// `state_size` bytes from `map` written over the fresh mapping's zeroes. Any block the owner
// holds now is unmapped (only state the restored game cannot reference lived there), and a
// retired mapping under the address is replaced. Returns the block's base.
unsigned char *ArenaBlockRestore(const void *owner, unsigned char *map, std::size_t map_size, std::size_t lead,
                                 const unsigned char *state, std::size_t state_size);

// Zeroes every live block whose owner is not in `keep`, so pointers the restored game cannot hold
// read zeroes as retired blocks do.
void ArenaZeroOtherBlocks(const void *const *keep, std::size_t count);

// Retired blocks count: the game can still hold a pointer into one.
bool ArenaContains(const void *pointer);

// Takes the bases rather than reading the blocks, so tests drive the check ArenaBlock runs.
void ArenaRequireOneAbove4GiB(const void *const *bases, std::size_t count);

[[noreturn]] void ArenaOverflow(const void *arena, int used, int limit);

// A name tells the log which carve ran dry. A char * binds here, not to the pointer overload,
// so cast arena addresses to const void * before passing them.
[[noreturn]] void ArenaOverflow(const char *name, int used, int limit);

// The image is far smaller than 2 GiB, so of the addresses that end in the 32 bits the game kept,
// the one nearest an address inside the image is the one that was truncated. Exact wherever the
// image sits.
inline std::uintptr_t WidenNear(std::uintptr_t anchor, std::uint32_t low) {
    constexpr std::uintptr_t kWindow = std::uintptr_t{1} << 32;
    std::uintptr_t           candidate = (anchor & ~(kWindow - 1)) | low;
    if (candidate > anchor && candidate - anchor > kWindow / 2 && candidate >= kWindow) {
        candidate -= kWindow;
    } else if (candidate < anchor && anchor - candidate > kWindow / 2) {
        candidate += kWindow;
    }
    return candidate;
}

// A pointer into the executable's image (a global, a static, a literal) back from the int the game
// stored it in; null for 0.
void *PortImagePointer(std::int32_t truncated);
