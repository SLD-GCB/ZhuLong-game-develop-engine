// 烛龙 (ZhuLong) - GPU layer common types.

#pragma once

#include <cstdint>

namespace zlong::gpu {

/// A GPU virtual address, as submitted by the guest driver. Distinct from the
/// CPU's virtual addresses: GPU addresses are translated by the GPU's own page
/// tables (GpuAddressSpace), not by ram::Mmu.
using GpuVAddr = std::uint64_t;

/// A guest physical address, i.e. an address in the space modelled by
/// zlong::ram::PhysicalMemory.
using GuestPa = std::uint64_t;

using ChannelId = std::uint32_t;
using SyncpointId = std::uint32_t;

inline constexpr ChannelId kInvalidChannel = 0xFFFF'FFFFu;
inline constexpr SyncpointId kInvalidSyncpoint = 0xFFFF'FFFFu;

/// Which engine a channel targets. Order matches the engine array index.
enum class ChannelType : std::uint8_t {
    ThreeD = 0,
    TwoD = 1,
    Compute = 2,
    DmaCopy = 3,
};

inline constexpr std::size_t kChannelTypeCount = 4;

}  // namespace zlong::gpu
