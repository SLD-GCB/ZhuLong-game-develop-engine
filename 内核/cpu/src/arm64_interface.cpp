#include "zlong/cpu/arm64_interface.h"

#include <cstddef>
#include <cstdint>

#include "zlong/cpu/core.h"
#include "zlong/cpu/cpu.h"

namespace zlong::cpu {

std::optional<std::uint32_t> Arm64Interface::MemoryReadCode(Dynarmic::A64::VAddr vaddr) {
    // Returning nullopt makes dynarmic raise Exception::NoExecuteFault, which
    // comes back through ExceptionRaised -> Core::OnInstructionAbort.
    return core_.Memory().TryFetch32(vaddr);
}

std::uint8_t Arm64Interface::MemoryRead8(Dynarmic::A64::VAddr vaddr) {
    if (const auto value = core_.Memory().TryRead8(vaddr)) {
        return *value;
    }
    ReportFault(vaddr, 1, /*is_write=*/false, /*is_fetch=*/false);
    return 0;
}

std::uint16_t Arm64Interface::MemoryRead16(Dynarmic::A64::VAddr vaddr) {
    if (const auto value = core_.Memory().TryRead16(vaddr)) {
        return *value;
    }
    ReportFault(vaddr, 2, false, false);
    return 0;
}

std::uint32_t Arm64Interface::MemoryRead32(Dynarmic::A64::VAddr vaddr) {
    if (const auto value = core_.Memory().TryRead32(vaddr)) {
        return *value;
    }
    ReportFault(vaddr, 4, false, false);
    return 0;
}

std::uint64_t Arm64Interface::MemoryRead64(Dynarmic::A64::VAddr vaddr) {
    if (const auto value = core_.Memory().TryRead64(vaddr)) {
        return *value;
    }
    ReportFault(vaddr, 8, false, false);
    return 0;
}

Dynarmic::A64::Vector Arm64Interface::MemoryRead128(Dynarmic::A64::VAddr vaddr) {
    if (const auto value = core_.Memory().TryRead128(vaddr)) {
        return *value;
    }
    ReportFault(vaddr, 16, false, false);
    return {0, 0};
}

void Arm64Interface::MemoryWrite8(Dynarmic::A64::VAddr vaddr, std::uint8_t value) {
    if (!core_.Memory().TryWrite8(vaddr, value)) {
        ReportFault(vaddr, 1, /*is_write=*/true, false);
    }
}

void Arm64Interface::MemoryWrite16(Dynarmic::A64::VAddr vaddr, std::uint16_t value) {
    if (!core_.Memory().TryWrite16(vaddr, value)) {
        ReportFault(vaddr, 2, true, false);
    }
}

void Arm64Interface::MemoryWrite32(Dynarmic::A64::VAddr vaddr, std::uint32_t value) {
    if (!core_.Memory().TryWrite32(vaddr, value)) {
        ReportFault(vaddr, 4, true, false);
    }
}

void Arm64Interface::MemoryWrite64(Dynarmic::A64::VAddr vaddr, std::uint64_t value) {
    if (!core_.Memory().TryWrite64(vaddr, value)) {
        ReportFault(vaddr, 8, true, false);
    }
}

void Arm64Interface::MemoryWrite128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value) {
    if (!core_.Memory().TryWrite128(vaddr, value)) {
        ReportFault(vaddr, 16, true, false);
    }
}

// Once dynarmic's exclusive monitor has accepted the reservation, these perform
// the actual store. Comparing against `expected` (the value snapshotted by the
// LDXR) additionally rejects the case where another agent changed the location
// without going through the monitor, which would otherwise let STXR "succeed"
// on stale data.

bool Arm64Interface::MemoryWriteExclusive8(Dynarmic::A64::VAddr vaddr, std::uint8_t value,
                                           std::uint8_t expected) {
    const auto current = core_.Memory().TryRead8(vaddr);
    if (!current || *current != expected) {
        return false;
    }
    return core_.Memory().TryWrite8(vaddr, value);
}

bool Arm64Interface::MemoryWriteExclusive16(Dynarmic::A64::VAddr vaddr, std::uint16_t value,
                                            std::uint16_t expected) {
    const auto current = core_.Memory().TryRead16(vaddr);
    if (!current || *current != expected) {
        return false;
    }
    return core_.Memory().TryWrite16(vaddr, value);
}

bool Arm64Interface::MemoryWriteExclusive32(Dynarmic::A64::VAddr vaddr, std::uint32_t value,
                                            std::uint32_t expected) {
    const auto current = core_.Memory().TryRead32(vaddr);
    if (!current || *current != expected) {
        return false;
    }
    return core_.Memory().TryWrite32(vaddr, value);
}

bool Arm64Interface::MemoryWriteExclusive64(Dynarmic::A64::VAddr vaddr, std::uint64_t value,
                                            std::uint64_t expected) {
    const auto current = core_.Memory().TryRead64(vaddr);
    if (!current || *current != expected) {
        return false;
    }
    return core_.Memory().TryWrite64(vaddr, value);
}

bool Arm64Interface::MemoryWriteExclusive128(Dynarmic::A64::VAddr vaddr,
                                             Dynarmic::A64::Vector value,
                                             Dynarmic::A64::Vector expected) {
    const auto current = core_.Memory().TryRead128(vaddr);
    if (!current || *current != expected) {
        return false;
    }
    return core_.Memory().TryWrite128(vaddr, value);
}

void Arm64Interface::InterpreterFallback(Dynarmic::A64::VAddr pc, std::size_t /*num_instructions*/) {
    // dynarmic sends un-translated instructions here, and its Interpret terminal
    // sets jit_state.pc to the instruction's own address -- so `pc` is the
    // instruction itself, not the next one.
    //
    // This is the channel for guest-driven memory management: dynarmic's A64
    // MRS/MSR only implements nine registers and everything else lands here,
    // including TTBR0_EL1 / TTBR1_EL1 / TCR_EL1 / MAIR_EL1 / SCTLR_EL1. The
    // memory layer owns those, so let it have a go before giving up.
    if (const auto word = core_.Memory().TryFetch32(pc)) {
        const std::uint32_t instruction = *word;
        const std::uint32_t rt = instruction & 0x1Fu;
        const std::uint64_t rt_value = (rt == 31) ? 0 : core_.GetJit().GetRegister(rt);

        const auto result =
            core_.Memory().HandleSystemInstruction(core_.Index(), pc, instruction, rt_value);
        if (result.kind == SystemInsnResult::Kind::Handled) {
            // Rt == 31 is XZR in these encodings; SetRegister(31) would write SP.
            if (result.write_rt != 0xFF && result.write_rt != 31) {
                core_.GetJit().SetRegister(result.write_rt, result.write_value);
            }
            core_.GetJit().SetPC(pc + 4);
            return;
        }
    }

    // Genuinely unsupported (e.g. a half-precision SIMD form dynarmic skips):
    // there is no fallback interpreter, and recursively entering Run()/Step()
    // is forbidden. Report and let the kernel decide.
    core_.OnTrapped(pc, UndefinedReason::InterpreterFallback);
}

void Arm64Interface::CallSVC(std::uint32_t swi) {
    SvcCall call;
    call.swi = swi;
    // dynarmic does SetPC(PC + 4) before CallSupervisor, so the SVC's own
    // address is PC - 4.
    call.pc = core_.GetJit().GetPC() - 4;
    core_.ReportSvc(call);
    core_.GetJit().HaltExecution(kHaltSvc);
}

void Arm64Interface::ExceptionRaised(Dynarmic::A64::VAddr pc, Dynarmic::A64::Exception exception) {
    using Exception = Dynarmic::A64::Exception;

    switch (exception) {
    case Exception::WaitForEvent:
        core_.OnWaitForEvent();
        return;
    case Exception::WaitForInterrupt:
        core_.OnWaitForInterrupt();
        return;
    case Exception::SendEvent:
        core_.OnSendEvent();
        return;
    case Exception::SendEventLocal:
        core_.OnSendEventLocal();
        return;
    case Exception::Yield:
        core_.OnYield();
        return;
    case Exception::Breakpoint:
        core_.OnTrapped(pc, UndefinedReason::Breakpoint);
        return;
    case Exception::UnallocatedEncoding:
        core_.OnTrapped(pc, UndefinedReason::UnallocatedEncoding);
        return;
    case Exception::ReservedValue:
        core_.OnTrapped(pc, UndefinedReason::ReservedValue);
        return;
    case Exception::UnpredictableInstruction:
        core_.OnTrapped(pc, UndefinedReason::Unpredictable);
        return;
    case Exception::NoExecuteFault:
        core_.OnInstructionAbort(pc);
        return;
    }
    core_.OnTrapped(pc, UndefinedReason::Other);
}

void Arm64Interface::AddTicks(std::uint64_t ticks) {
    core_.AddTicks(ticks);
}

std::uint64_t Arm64Interface::GetTicksRemaining() {
    return core_.TicksRemaining();
}

std::uint64_t Arm64Interface::GetCNTPCT() {
    return core_.GetCpu().GetCNTPCT();
}

void Arm64Interface::ReportFault(Dynarmic::A64::VAddr vaddr, std::size_t size, bool is_write,
                                 bool is_fetch) {
    MemoryFault fault;
    fault.address = vaddr;
    // The accessors only carry success/failure; ask the memory layer why, so the
    // kernel can tell a translation fault from a permission fault and at which
    // level.
    const FaultInfo info = core_.Memory().DescribeFault(vaddr, is_write, is_fetch);
    fault.kind = info.kind;
    fault.level = info.level;
    fault.is_write = is_write;
    fault.is_fetch = is_fetch;
    fault.size_bytes = static_cast<std::uint8_t>(size);

    core_.ReportMemoryFault(fault);
    // Halt cleanly instead of throwing: this runs underneath JIT-generated
    // frames that have no unwind information.
    core_.GetJit().HaltExecution(Dynarmic::HaltReason::MemoryAbort);
}

}  // namespace zlong::cpu
