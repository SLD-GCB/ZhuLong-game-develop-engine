// 烛龙 (ZhuLong) - the GPU layer's callback seam into the kernel.
//
// Policy lives in the kernel: the GPU layer reports events and never decides
// whether a guest core should be interrupted. (Same shape as cpu::CpuHost.)

#pragma once

#include <cstdint>

#include "zlong/gpu/fault.h"
#include "zlong/gpu/types.h"

namespace zlong::gpu {

class GpuHost {
public:
    virtual ~GpuHost() = default;

    /// A syncpoint reached (or passed) the given value. The kernel decides what
    /// that means: unblock a parked core (already done for parkers), or
    /// AssertInterrupt(Irq) if the core is in WFI instead of parked.
    virtual void OnSyncpointSignal(SyncpointId /*id*/, std::uint32_t /*value*/) {}

    /// A GPU-side fault. Reported for diagnosis; not necessarily fatal.
    virtual void OnGpuFault(const GpuFault& /*fault*/) {}
};

}  // namespace zlong::gpu
