// 烛龙 (ZhuLong) - building a stage-1 address space.
//
// Mmu only walks page tables. This is the other half: it allocates tables,
// writes descriptors and programs the translation regime, so a caller can map a
// guest virtual range onto physical memory.
//
// The descriptor format lives in mmu.h and the builders below use those same
// constants, so the mapping side and the walk cannot disagree about bits[1:0].
//
// Scope: 4 KB granule, TTBR0 (and TTBR1 pointed at the same root), one flat
// address space. That is what running under HLE needs -- the guest's own EL1
// kernel does not execute, so there is nobody to switch regimes.

#pragma once

#include <cstdint>

#include "zlong/ram/mmu.h"
#include "zlong/ram/ram.h"

namespace zlong::ram {

/// Descriptor attributes and builders, spelled out so a call site reads as what
/// it writes.
namespace pt {

inline constexpr std::uint64_t kAf = kDescriptorAfBit;
inline constexpr std::uint64_t kUxn = kDescriptorUxnBit;
inline constexpr std::uint64_t kPxn = kDescriptorPxnBit;

/// AP[2:1] live at bits[7:6]: bit 6 is read-only, bit 7 is EL0-accessible.
inline constexpr std::uint64_t Ap(bool el0_accessible, bool read_only) {
    return (el0_accessible ? (1ULL << 7) : 0ULL) | (read_only ? (1ULL << 6) : 0ULL);
}

inline constexpr std::uint64_t RwEl0() { return kAf | Ap(true, false); }
inline constexpr std::uint64_t RoEl0() { return kAf | Ap(true, true); }
inline constexpr std::uint64_t RwEl1Only() { return kAf | Ap(false, false); }
inline constexpr std::uint64_t RoEl1Only() { return kAf | Ap(false, true); }

/// A table descriptor, levels 0..2: bits[1:0] == 0b11.
inline constexpr std::uint64_t Table(std::uint64_t next) {
    return (next & kDescriptorAddressMask) | kDescriptorValidBit | kDescriptorTypeBit;
}

/// A level-3 page: bits[1:0] == 0b11.
inline constexpr std::uint64_t Page(std::uint64_t pa, std::uint64_t attrs) {
    return (pa & kDescriptorAddressMask) | kDescriptorValidBit | kDescriptorTypeBit | attrs;
}

/// A block, levels 0..2 only: bits[1:0] == 0b01.
inline constexpr std::uint64_t Block(std::uint64_t pa, std::uint64_t attrs) {
    return (pa & kDescriptorAddressMask) | kDescriptorValidBit | attrs;
}

}  // namespace pt

class AddressSpace {
public:
    explicit AddressSpace(Ram& ram);

    /// Point TTBR0 and TTBR1 at a fresh level-0 table and turn the MMU on.
    /// A t0sz of 16 gives a 48-bit VA, which covers everything DRAM sits at.
    bool enable(std::uint32_t t0sz = 16);

    std::uint64_t allocate_page();

    /// Make sure the tables above `level` exist for `va`, and return the table at
    /// `level`. Zero when a block already covers `va`, or when allocation failed.
    ///
    /// Public because a caller building a deliberate fault has to install a leaf
    /// -- or leave an entry invalid -- at a chosen level.
    std::uint64_t table_for(std::uint64_t va, int level);

    bool map_page(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs);
    bool map_block_2mb(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs);
    bool map_block_1gb(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs);

    /// Drop the level-3 page for `va`, leaving the tables in place. The entry is
    /// zeroed, which the walk classifies as invalid -- so the format stays in one
    /// place instead of being written out again here.
    bool unmap_page(std::uint64_t va);

    std::uint64_t root0() const noexcept { return root0_; }

private:
    void store(std::uint64_t entry_pa, std::uint64_t descriptor);

    Ram& ram_;
    std::uint64_t root0_ = 0;
};

}  // namespace zlong::ram
