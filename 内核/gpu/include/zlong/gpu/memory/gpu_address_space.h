// 烛龙 (ZhuLong) - the GPU's own address space.
//
// The console is unified memory: there is no separate VRAM, so an allocation's
// backing store is guest physical memory and a GPU address is a way of NAMING that
// memory, not a second copy of it. What this class needs to be, then, is a mapping
// from GPU address to guest physical address and nothing more.
//
// It does have to be a real mapping, though. Everything the guest submits names
// memory this way, and a command stream that names an address nobody mapped is a
// fault -- so the resolve is a lookup, not arithmetic at each point of use. A page
// table would go in exactly here, and nothing outside this class can tell which one
// is behind it.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "zlong/gpu/types.h"

namespace zlong::gpu {

class GpuAddressSpace {
public:
    /// Where GPU addresses begin. Far above any guest physical layout, so a GPU
    /// address and a physical address cannot be confused -- and if one is ever passed
    /// where the other belongs, the resolve misses instead of quietly working.
    static constexpr GpuVAddr kBase = 0x0000'0100'0000'0000ull;

    /// Name a physical range for the GPU. Returns the address the GPU uses for it.
    /// Mapping the same range twice returns the address it already has.
    GpuVAddr Map(GuestPa physical, std::uint64_t size);

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

    /// Forget the mapping that starts at `address`. True when one was there.
    bool Unmap(GpuVAddr address);
    void Clear();

    std::size_t mapping_count() const noexcept { return mappings_.size(); }

private:
    struct Mapping {
        GpuVAddr address = 0;
        GuestPa physical = 0;
        std::uint64_t size = 0;
    };

    std::vector<Mapping> mappings_;
    /// Where the next unclaimed GPU address comes from.
    GpuVAddr next_ = kBase;
};

}  // namespace zlong::gpu
