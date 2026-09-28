// 烛龙 (ZhuLong) - the kernel/HLE seam of the CPU layer.
//
// This is the ONLY place the kernel plugs into the CPU. The CPU layer contains
// no syscall numbers, no HLE, no guest exception model: it reports events and
// the kernel decides what they mean.

#pragma once

#include "zlong/cpu/types.h"

namespace zlong::cpu {

class Core;

class CpuHost {
public:
    virtual ~CpuHost() = default;

    /// A guest core executed SVC. Called on that core's own thread, and only
    /// while the core is NOT inside dynarmic's Run() -- so a blocking syscall
    /// handler may call Core::ParkCurrentThread() safely.
    ///
    /// Convention: dynarmic has already advanced PC past the SVC. The handler
    /// must not modify PC except for guest exception-return sequences; on
    /// return the core resumes from the current JIT state.
    virtual void OnSvc(Core& core, const SvcCall& call) = 0;

    /// Deliver a pending IRQ/FIQ. Called on the core's own thread between
    /// Run() calls; the kernel performs the guest exception entry
    /// (ELR_EL1/SPSR_EL1/VBAR, PSTATE masking) via Core::GetJit().
    virtual void OnInterrupt(Core& core, Interrupt interrupt) = 0;

    /// A guest data access hit unmapped/protected memory. The core has already
    /// been halted cleanly; the kernel decides whether this becomes a guest
    /// data abort or kills the emulation.
    virtual void OnMemoryFault(Core& /*core*/, const MemoryFault& /*fault*/) {}

    /// A guest instruction could not be translated (unallocated encoding,
    /// reserved value, BRK, or a dynarmic interpreter fallback).
    virtual void OnUndefinedInstruction(Core& /*core*/, const UndefinedInsn& /*insn*/) {}

    /// The core's per-frame cycle budget ran out. Advisory scheduling hook;
    /// the default does nothing.
    virtual void OnTimesliceExpired(Core& /*core*/) {}
};

}  // namespace zlong::cpu
