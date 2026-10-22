#include "platform/arena.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <elf.h>
#include <link.h>
#endif

#include "platform/memory.hpp"

namespace {

// The reservation every block is carved from: two gibibytes of address space, committed only where
// the blocks are. Retail's arenas were a few dozen megabytes; four times retail, a retired block's
// worth of history and a wide margin fit with room to spare.
constexpr std::size_t kRegionBytes = std::size_t{2} << 30;

struct Block {
    const void *owner;
    ArenaMemory memory;
};

std::vector<Block> g_blocks;
// Blocks an owner outgrew stay mapped: retail's memory never went away, and a pointer the game
// kept into an old arena reads zeroes rather than faulting.
std::vector<Block> g_retired;

ArenaRegionReservation g_region;
bool                   g_region_tried = false;

std::size_t RoundUp(std::size_t value, std::size_t to) { return (value + to - 1) / to * to; }

bool InRegion(const ArenaMemory &memory) {
    return g_region.map != nullptr && memory.map >= g_region.map && memory.map < g_region.map + g_region.size;
}

std::size_t RegionOffset(const ArenaMemory &memory) { return static_cast<std::size_t>(memory.map - g_region.map); }

// The first place a new block can go: past every tracked range, so nothing a state restore puts
// back is ever carved over.
std::size_t RegionWatermark() {
    std::size_t end = 0;
    for (const Block &block : g_blocks) {
        end = std::max(end, RegionOffset(block.memory) + block.memory.map_size);
    }
    for (const Block &block : g_retired) {
        end = std::max(end, RegionOffset(block.memory) + block.memory.map_size);
    }
    return end;
}

ArenaMemory RegionCarve(std::size_t bytes, std::size_t lead) {
    std::size_t page = ArenaMemoryPageSize();
    std::size_t capacity = RoundUp(bytes == 0 ? 64 : bytes, 64);
    std::size_t span = RoundUp(lead + capacity, page);
    std::size_t map_size = span + page;
    std::size_t offset = RoundUp(RegionWatermark(), 64);
    if (offset + map_size > g_region.size) {
        std::fprintf(stderr, "arena: the fixed reservation ran out at %zu of %zu bytes\n", offset, g_region.size);
        std::abort();
    }
    auto *map = g_region.map + offset;
    ArenaRegionCommit(map, span);
    ArenaRegionProtect(map + span, page);
    return {map, map_size, map + span - capacity, capacity, lead};
}

void RegionTryReserve() {
    if (g_region_tried) {
        return;
    }
    g_region_tried = true;
    g_region = ArenaRegionReserve(kRegionBytes);
    if (g_region.map == nullptr) {
        std::fprintf(stderr, "arena: no fixed reservation was free; save states hold this session only\n");
    }
}

bool MapHolds(const std::vector<Block> &blocks, std::uintptr_t address) {
    for (const Block &block : blocks) {
        const auto start = reinterpret_cast<std::uintptr_t>(block.memory.map);
        if (address - start < block.memory.map_size) {
            return true;
        }
    }
    return false;
}

// Every 32-bit truncation the game makes of an arena pointer is widened; one left behind only shows
// when the pointer has bits above 32, so a debug build refuses to run where none would.
void CheckSomeBlockHigh() {
#ifndef NDEBUG
    std::vector<const void *> bases;
    bases.reserve(g_blocks.size());
    for (const Block &block : g_blocks) {
        bases.push_back(block.memory.base);
    }
    ArenaRequireOneAbove4GiB(bases.data(), bases.size());
#endif
}

// Retires a live block: zeroed, so the game's stale pointers read zeroes, and remembered, since a
// pointer into it must not fault.
void Retire(Block &block) {
    ArenaMemoryZero(block.memory);
    g_retired.push_back(block);
}

// The block a live block of `owner` would be carved as when the one it holds no longer fits.
ArenaMemory NewBlock(const void *owner, std::size_t bytes, std::size_t lead) {
    if (g_region.map != nullptr) {
        return RegionCarve(bytes, lead);
    }
    return ArenaMemoryMap(bytes, lead);
}

} // namespace

unsigned char *ArenaBlock(const void *owner, std::size_t bytes, std::size_t lead) {
    RegionTryReserve();
    std::size_t capacity = RoundUp(bytes == 0 ? 64 : bytes, 64);
    for (Block &block : g_blocks) {
        if (block.owner != owner) {
            continue;
        }
        if (block.memory.capacity >= capacity && block.memory.lead >= lead) {
            return block.memory.base;
        }
        Retire(block);
        block = Block{owner, NewBlock(owner, bytes, lead)};
        CheckSomeBlockHigh();
        return block.memory.base;
    }
    g_blocks.push_back(Block{owner, NewBlock(owner, bytes, lead)});
    CheckSomeBlockHigh();
    return g_blocks.back().memory.base;
}

void ArenaClearAll() {
    for (const Block &block : g_blocks) {
        ArenaMemoryZero(block.memory);
    }
}

std::size_t ArenaBlockCount() {
    return g_blocks.size();
}

ArenaBlockInfo ArenaBlockInfoAt(std::size_t index) {
    const Block        &block = g_blocks.at(index);
    const ArenaMemory  &memory = block.memory;
    std::size_t         offset = 0;
    if (InRegion(memory)) {
        offset = RegionOffset(memory);
    } else {
        offset = reinterpret_cast<std::uintptr_t>(memory.map);
    }
    return {block.owner, offset, memory.map_size - ArenaMemoryPageSize(), memory.lead};
}

unsigned char *ArenaRegionBase() {
    return g_region.map;
}

std::size_t ArenaRegionSize() {
    return g_region.map != nullptr ? g_region.size : 0;
}

unsigned char *ArenaBlockRestore(const void *owner, std::size_t offset, std::size_t usable, std::size_t lead,
                                 const unsigned char *state, std::size_t state_size) {
    if (g_region.map == nullptr) {
        std::fprintf(stderr, "arena: a state restore needs the fixed reservation, which this session has not\n");
        std::abort();
    }
    std::size_t page = ArenaMemoryPageSize();
    std::size_t span = RoundUp(usable, page);
    std::size_t map_size = span + page;
    if (offset % 64 != 0 || lead > usable || state_size != usable || offset + map_size > g_region.size) {
        std::fprintf(stderr, "arena: restore of a block at offset %zu does not fit the reservation\n", offset);
        std::abort();
    }
    auto *map = g_region.map + offset;
    // Whatever holds the block's place now is state the restored game cannot reference: zeroed and
    // retired, and the owner's other blocks with it.
    for (std::size_t i = 0; i < g_retired.size(); i++) {
        Block          &block = g_retired[i];
        const auto      block_start = reinterpret_cast<std::uintptr_t>(block.memory.map);
        const auto      range_start = reinterpret_cast<std::uintptr_t>(map);
        const auto      range_end = range_start + map_size;
        if (block_start < range_end && range_start < block_start + block.memory.map_size) {
            ArenaMemoryZero(block.memory);
        }
    }
    for (std::size_t i = 0; i < g_blocks.size();) {
        Block          &block = g_blocks[i];
        const auto      block_start = reinterpret_cast<std::uintptr_t>(block.memory.map);
        const auto      range_start = reinterpret_cast<std::uintptr_t>(map);
        const auto      range_end = range_start + map_size;
        bool            overlaps = block_start < range_end && range_start < block_start + block.memory.map_size;
        if (overlaps || block.owner == owner) {
            Retire(block);
            g_blocks.erase(g_blocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
    // Fresh zeroes under the state, and the block's guard page closed again, whatever held these
    // pages before.
    ArenaRegionDecommit(map, span);
    ArenaRegionCommit(map, span);
    ArenaRegionProtect(map + span, page);
    std::memcpy(map, state, state_size);
    // A retired range fully under the block now reads the block's own memory, not the zeroes it
    // promises, so it stops being retired.
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                   [&](const Block &block) {
                                       return block.memory.map >= map &&
                                              block.memory.map + block.memory.map_size <= map + map_size;
                                   }),
                    g_retired.end());
    g_blocks.push_back({owner, {map, map_size, map + span - (usable - lead), usable - lead, lead}});
    return map + span - (usable - lead);
}

void ArenaZeroOtherBlocks(const void *const *keep, std::size_t count) {
    std::vector<Block> kept;
    for (Block &block : g_blocks) {
        bool wanted = false;
        for (std::size_t i = 0; i < count; i++) {
            if (keep[i] == block.owner) {
                wanted = true;
                break;
            }
        }
        if (wanted) {
            kept.push_back(block);
        } else {
            Retire(block);
        }
    }
    g_blocks = std::move(kept);
}

bool ArenaContains(const void *pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return MapHolds(g_blocks, address) || MapHolds(g_retired, address);
}

void ArenaRequireOneAbove4GiB(const void *const *bases, std::size_t count) {
#ifndef NDEBUG
    for (std::size_t i = 0; i < count; i++) {
        if (IsAbove4GiB(bases[i])) {
            return;
        }
    }
    std::fprintf(stderr, "arena: none lies above 4 GiB, where a truncated pointer would show\n");
    std::abort();
#endif
}

void *PortImagePointer(std::int32_t truncated) {
    static const char anchor = 0;
    if (truncated == 0) {
        return nullptr;
    }
    void *pointer = reinterpret_cast<void *>(WidenNear(reinterpret_cast<std::uintptr_t>(&anchor),
                                                       static_cast<std::uint32_t>(truncated)));
    // A state taken in an earlier session carries the low bits of an image that sat elsewhere; the
    // widened address then falls outside this image, and a null beats a pointer into someone else's
    // memory. The game treats a null wind as no wind.
    static std::uintptr_t image_start = 0;
    static std::uintptr_t image_end = 0;
    if (image_end == 0) {
#ifdef _WIN32
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&anchor), &module)) {
            auto *dos = static_cast<IMAGE_DOS_HEADER *>(static_cast<void *>(module));
            auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<std::uint8_t *>(module) + dos->e_lfanew);
            image_start = reinterpret_cast<std::uintptr_t>(module);
            image_end = image_start + nt->OptionalHeader.SizeOfImage;
        } else {
            image_end = 1;
        }
#else
        struct {
            std::uintptr_t start = ~std::uintptr_t{0};
            std::uintptr_t end = 0;
            const void    *self = &anchor;
        } scan;
        dl_iterate_phdr(
            [](dl_phdr_info *info, std::size_t, void *out) -> int {
                auto *range = static_cast<decltype(&scan)>(out);
                for (int i = 0; i < info->dlpi_phnum; i++) {
                    if (info->dlpi_phdr[i].p_type != PT_LOAD) {
                        continue;
                    }
                    std::uintptr_t start = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
                    std::uintptr_t end = start + info->dlpi_phdr[i].p_memsz;
                    if (reinterpret_cast<std::uintptr_t>(range->self) < start ||
                        reinterpret_cast<std::uintptr_t>(range->self) >= end) {
                        continue;
                    }
                    for (int j = 0; j < info->dlpi_phnum; j++) {
                        if (info->dlpi_phdr[j].p_type != PT_LOAD) {
                            continue;
                        }
                        range->start = std::min(range->start, static_cast<std::uintptr_t>(info->dlpi_addr) +
                                                                      static_cast<std::uintptr_t>(info->dlpi_phdr[j].p_vaddr));
                        range->end = std::max(range->end, static_cast<std::uintptr_t>(info->dlpi_addr) +
                                                              static_cast<std::uintptr_t>(info->dlpi_phdr[j].p_vaddr) +
                                                              static_cast<std::uintptr_t>(info->dlpi_phdr[j].p_memsz));
                    }
                    return 1;
                }
                return 0;
            },
            &scan);
        image_start = scan.start;
        image_end = scan.end == 0 ? 1 : scan.end;
#endif
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < image_start || address >= image_end) {
        return nullptr;
    }
    return pointer;
}

void ArenaOverflow(const void *arena, int used, int limit) {
    std::fprintf(stderr, "arena %p: allocation overflow, %d of %d quadwords\n", arena, used, limit);
    std::abort();
}

void ArenaOverflow(const char *name, int used, int limit) {
    std::fprintf(stderr, "arena %s: allocation overflow, %d of %d quadwords\n", name, used, limit);
    std::abort();
}