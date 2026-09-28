// 烛龙 (ZhuLong) - the guest address space, as the kernel sees it.
//
// RAM's AddressSpace builds page tables. It cannot answer the questions a
// syscall has to answer -- "is this range mine", "is it already mapped", "what
// is it allowed to be used for" -- because a page table only stores the last of
// those. This is the layer that tracks the regions, so a mapping request can be
// checked against what the process actually owns.
//
// One address space, not one per process: under HLE the guest's own EL1 kernel
// does not run, so nothing switches regimes. That matches how the RAM layer
// already treats the translation control registers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zlong/ram/address_space.h"
#include "zlong/ram/ram.h"

namespace zlong::service {

/// What a mapped range may be used for.
inline constexpr std::uint32_t kMemoryRead = 1u << 0;
inline constexpr std::uint32_t kMemoryWrite = 1u << 1;
inline constexpr std::uint32_t kMemoryExecute = 1u << 2;

/// Where guest virtual addresses start. Chosen clear of the low mappings a guest
/// might still make, and inside the 48-bit range a t0sz of 16 covers.
inline constexpr std::uint64_t kFirstGuestAddress = 0x0000'0000'1000'0000ull;
/// Anything at or above this is outside what the configured regime translates.
inline constexpr std::uint64_t kLastGuestAddress = 0x0000'7FFF'FFFF'FFFFull;

class KAddressSpace {
public:
    KAddressSpace(ram::Ram& ram, ram::PhysicalMemory& physical);

    KAddressSpace(const KAddressSpace&) = delete;
    KAddressSpace& operator=(const KAddressSpace&) = delete;

    /// Point the MMU at a fresh root table and turn it on. Idempotent.
    bool Enable();

    struct Region {
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        std::uint32_t permissions = 0;
        bool mapped = false;
        bool heap = false;
    };

    /// Set a virtual range aside with no backing yet. Refuses an overlap, an
    /// unaligned or empty range, and one outside the translatable range.
    bool Reserve(std::uint64_t base, std::uint64_t size, std::string& error);

    /// Give a reserved range physical pages and map it. Refuses a range that was
    /// never reserved, or one that is already mapped.
    bool Map(std::uint64_t base, std::uint64_t size, std::uint32_t permissions,
             std::string& error);

    /// Map a reserved range onto a physical range the caller already owns.
    ///
    /// Boot uses this to place the entry module at a known physical address, and
    /// a test uses it to keep the code it is running mapped once the MMU is on.
    bool MapTo(std::uint64_t base, std::uint64_t physical_base, std::uint64_t size,
               std::uint32_t permissions, std::string& error);

    /// Reserve and map `size` bytes at `base`, then copy `bytes` into them. The
    /// last page is padded, so `size` need not be a multiple of the page size.
    ///
    /// The copy goes through the translation this just installed, at EL1, and
    /// writes the physical page directly. That is deliberate: a loader fills a
    /// page it has mapped read-only or execute-only, and a store from EL0 would
    /// -- correctly -- fault on it.
    ///
    /// The physical pages come from the allocator, so the image cannot land on
    /// the kernel's page tables. See PhysicalMemory::allocate for why a
    /// hard-coded physical address can.
    ///
    /// Writes the physical page directly, which means it does NOT report the
    /// write as guest code, so a core that had already translated this memory
    /// would keep running its old translation. Boot loads before any core runs.
    /// Loading over memory a core has executed needs the guest write path
    /// instead (Ram::TryWrite*, which reaches the code-write observer).
    bool LoadImage(std::uint64_t base, const void* bytes, std::size_t size,
                   std::uint32_t permissions, std::string& error);

    /// Drop a mapping.
    ///
    /// The physical pages are NOT handed back: the physical allocator is a bump
    /// allocator with no free. That is a real leak, it is bounded by how often a
    /// guest maps and unmaps, and it is written down here rather than left to be
    /// discovered.
    bool Unmap(std::uint64_t base, std::uint64_t size, std::string& error);

    /// Change the permissions of a mapped range.
    bool SetAttributes(std::uint64_t base, std::uint64_t size, std::uint32_t permissions,
                       std::string& error);

    /// Reserve a heap and return its base, or zero when it could not be made.
    std::uint64_t CreateHeap(std::uint64_t size);

    const Region* Find(std::uint64_t address) const;
    const std::vector<Region>& regions() const noexcept { return regions_; }
    std::size_t mapped_regions() const noexcept;
    bool enabled() const noexcept { return enabled_; }

    /// The first address no reservation covers. A loader puts what follows the
    /// image here, so the two cannot overlap.
    std::uint64_t next_free() const noexcept { return next_free_; }

private:
    /// The single region that wholly contains [base, base + size).
    Region* FindContaining(std::uint64_t base, std::uint64_t size);
    /// The checks both Map and MapTo need before either touches a page table.
    Region* PrepareMapping(std::uint64_t base, std::uint64_t size, std::string& error);

    ram::Ram& ram_;
    ram::PhysicalMemory& physical_;
    ram::AddressSpace space_;
    std::vector<Region> regions_;
    std::uint64_t next_free_ = kFirstGuestAddress;
    bool enabled_ = false;
};

}  // namespace zlong::service
