// 烛龙 (ZhuLong) - the GPU's own address space.
//
// The console is unified memory: there is no separate VRAM, so an allocation's
// backing store is guest physical memory and a GPU address is a way of NAMING that
// memory, not a second copy of it. What this class needs to be, then, is a mapping
// from GPU address to guest physical address -- plus the three things a real GPU
// address space carries on top of that, and which an emulator's GPU expects to
// find there:
//
//   ownership    the space is per process; a process going away takes its mappings
//                with it, and a leaked range is a leak of guest memory.
//   allocation   addresses are handed out and returned, not merely derived from a
//                physical range. A title reserves GPU address space before it has
//                anything to put in it.
//   page counts  how many GPU-side caches hold each page. That count is what
//                decides whether the CPU must flush before it writes, so it is the
//                accounting the whole unified-memory story rests on.
//
// It does have to be a real mapping, though. Everything the guest submits names
// memory this way, and a command stream that names an address nobody mapped is a
// fault -- so the resolve is a lookup, not arithmetic at each point of use. A page
// table would go in exactly here, and nothing outside this class can tell which one
// is behind it.
//
// ONE DELIBERATE DIFFERENCE from the console-style device address space, and from
// the emulator this is being joined to: those translate a device address to a CPU
// *virtual* address and let the process page table carry it the rest of the way.
// This resolves straight to guest physical. The kernel owns the address space, so
// the second hop would only be a second owner of the same fact.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "zlong/gpu/types.h"

namespace zlong::gpu {

/// Which process a range belongs to.
///
/// Zero is the unowned id -- what a caller that does not care gets, and never
/// something RegisterProcess hands out -- so a default-constructed Asid means
/// "nobody claimed this" rather than accidentally naming the first process.
struct Asid {
    std::uint32_t id = 0;

    bool valid() const noexcept { return id != 0; }
    friend bool operator==(const Asid&, const Asid&) = default;
};

/// What the space tells the rest of the machine when a page's cache count crosses
/// zero.
///
/// Only the crossings are reported, because only they change anything: a page
/// going 1 -> 2 is still cached, and a page going 2 -> 1 is still not free to
/// write. Between crossings there is nothing for a CPU or a renderer to do, and
/// reporting every step would be noise the size of the traffic.
class CachedPageObserver {
public:
    virtual ~CachedPageObserver() = default;

    /// The page at `physical` is now held by at least one GPU-side cache.
    virtual void OnPageCached(GuestPa /*physical*/) {}
    /// Nothing on the GPU holds it any more. Safe for the CPU to write.
    virtual void OnPageUncached(GuestPa /*physical*/) {}
};

class GpuAddressSpace {
public:
    /// Where GPU addresses begin. Far above any guest physical layout, so a GPU
    /// address and a physical address cannot be confused -- and if one is ever passed
    /// where the other belongs, the resolve misses instead of quietly working.
    static constexpr GpuVAddr kBase = 0x0000'0100'0000'0000ull;

    /// The granularity an allocation and a page count both work at. The console's
    /// device page is 4 KiB and so is the kernel's, so a walk in one is a walk in the
    /// other and no conversion is ever needed.
    static constexpr std::uint64_t kPageBits = 12;
    static constexpr std::uint64_t kPageSize = 1ull << kPageBits;
    static constexpr std::uint64_t kPageMask = kPageSize - 1;

    // --- processes ----------------------------------------------------------

    /// Mint an id for a new process. Ids are never reused, so a stale Asid that
    /// outlives its process names nothing instead of naming the next one.
    Asid RegisterProcess();

    /// Drop every mapping this process owns, and its page counts with them. Returns
    /// how many mappings were dropped; zero is a normal answer for a process that
    /// reserved nothing.
    std::size_t UnregisterProcess(Asid asid);

    // --- the device allocator ----------------------------------------------

    /// Hand out `size` bytes of GPU address space, page-aligned and unbacked. Empty
    /// when the size is zero or so large that no gap could hold it.
    ///
    /// The range is reserved but names no memory yet: a title reserves first and
    /// binds later, and a resolve into a range that was never bound is a fault --
    /// which is exactly what an empty result from Resolve means.
    std::optional<GpuVAddr> Allocate(std::uint64_t size, Asid asid);

    /// Reserve one specific range. False when any part of it is already taken --
    /// refusing rather than relocating, because the caller named the address it
    /// wanted and a different one would be a different program.
    bool AllocateFixed(GpuVAddr address, std::uint64_t size, Asid asid);

    /// Return a reserved or mapped range to the allocator.
    ///
    /// `size` must be the one that was handed out. Freeing a sub-range would either
    /// drop mappings the caller still holds or leave a remainder in a state nobody
    /// described; splitting is refused rather than guessed at.
    bool Free(GpuVAddr address, std::uint64_t size);

    // --- naming physical memory --------------------------------------------

    /// Name a physical range for the GPU, allocating its address. Mapping the same
    /// range twice returns the address it already has.
    GpuVAddr Map(GuestPa physical, std::uint64_t size, Asid asid = {});

    /// Bind physical memory into a range that was reserved earlier. False when no
    /// single reservation covers the whole range, or when it is already bound.
    bool Bind(GpuVAddr address, GuestPa physical, std::uint64_t size);

    /// The physical memory a GPU address names. Empty when no single mapping covers
    /// the whole range -- which is a fault to report, not a reason to guess.
    std::optional<GuestPa> Resolve(GpuVAddr address, std::uint64_t size) const;

    /// The same, for a caller that only has an address and not a length. A page table
    /// lookup is per address, and this is the shape the command decoder needs.
    std::optional<GuestPa> Resolve(GpuVAddr address) const;

    /// The GPU address a physical range is already mapped at. Empty when it is not
    /// mapped: an address that was never handed out is not one to invent.
    std::optional<GpuVAddr> AddressOf(GuestPa physical, std::uint64_t size) const;

    /// The same, for a caller that has an address and not a length.
    std::optional<GpuVAddr> AddressOf(GuestPa physical) const;

    /// Forget the mapping that starts at `address`, and any page counts under it.
    /// True when one was there.
    bool Unmap(GpuVAddr address);

    // --- cached-page accounting --------------------------------------------

    void set_cached_page_observer(CachedPageObserver* observer) noexcept {
        cached_observer_ = observer;
    }
    CachedPageObserver* cached_page_observer() const noexcept { return cached_observer_; }

    /// Move every page in [address, address + size) by `delta`, reporting the pages
    /// whose count crossed zero.
    ///
    /// A range that is not mapped is skipped rather than refused: a cache telling us
    /// it dropped a page it held is not a fault, and the page may already have
    /// belonged to a process that went away.
    void UpdateCachedCount(GpuVAddr address, std::uint64_t size, int delta);

    /// One entry of a batched update.
    struct CachedRange {
        GpuVAddr address = 0;
        std::uint64_t size = 0;
    };

    /// The batch form, so a cache releasing a hundred pages makes one call instead
    /// of a hundred.
    void UpdateCachedBatch(std::span<const CachedRange> ranges, int delta);

    /// How many caches hold the page containing `address`. Zero for an unmapped page.
    std::int32_t cached_count(GpuVAddr address) const;

    // --- everything at once -------------------------------------------------

    void Clear();

    std::size_t mapping_count() const noexcept { return mappings_.size(); }
    /// Ranges currently held by a process, for diagnostics.
    std::size_t mapping_count(Asid asid) const;

private:
    /// One reserved range. It exists from Allocate onward; `backed` says whether it
    /// names physical memory yet. A reserved-but-unbacked range still occupies its
    /// addresses -- that is what reserving means -- but a resolve into it finds
    /// nothing.
    struct Mapping {
        GpuVAddr address = 0;
        std::uint64_t size = 0;
        GuestPa physical = 0;
        Asid asid{};
        bool backed = false;
    };

    /// Mappings, ordered by address. Every lookup and every gap search walks this in
    /// order, so the order is an invariant rather than a convenience: an insertion
    /// that breaks it breaks the allocator, silently.
    std::vector<Mapping> mappings_;
    /// Per-page cache counts, keyed by the GPU address of the page. A map rather than
    /// an array because the GPU address space is sparse and enormous -- the console's
    /// allocator can hand out an address near the top with nothing at all below it.
    std::map<GpuVAddr, std::int32_t> cached_pages_;

    CachedPageObserver* cached_observer_ = nullptr;
    std::uint32_t next_asid_ = 1;

    /// The mapping that covers exactly [address, address + size), or nullptr.
    const Mapping* FindCovering(GpuVAddr address, std::uint64_t size) const;
    Mapping* FindCovering(GpuVAddr address, std::uint64_t size);
    /// Where a mapping of `size` bytes can start without touching another one.
    std::optional<GpuVAddr> FindGap(std::uint64_t size) const;
    static bool Overlaps(const Mapping& mapping, GpuVAddr address, std::uint64_t size);
    /// Forget the cache counts under a range that is being dropped.
    void DropPageCounts(const Mapping& mapping);
};

}  // namespace zlong::gpu
