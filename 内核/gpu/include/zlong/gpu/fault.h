// 烛龙 (ZhuLong) - GPU faults.

#pragma once

#include <cstdint>
#include <string>

#include "zlong/gpu/types.h"

namespace zlong::gpu {

enum class GpuFaultKind : std::uint8_t {
    None,
    /// A GPU virtual address had no valid translation.
    AddressTranslation,
    /// The command stream referenced a method we do not implement. Reported, not
    /// fatal: larger content degrades instead of crashing.
    UnimplementedMethod,
    /// The command stream was structurally invalid (truncated, bad opcode).
    MalformedCommandStream,
    /// The host GPU device was lost.
    DeviceLost,
    Internal,
};

struct GpuFault {
    GpuFaultKind kind = GpuFaultKind::None;
    GpuVAddr address = 0;
    std::uint32_t method = 0;
    std::string detail;
};

}  // namespace zlong::gpu
