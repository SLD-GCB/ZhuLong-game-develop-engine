// 烛龙 (ZhuLong) - CPU layer common types.
//
// Deliberately dependency-free: nothing here includes dynarmic, so the rest of
// the kernel can talk about CPU state without pulling in the JIT.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace zlong::cpu {

using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;  // {low64, high64}

/// NVIDIA Tegra X1: 4x ARM Cortex-A57.
inline constexpr std::size_t kCoreCount = 4;

/// Generic-timer frequency (CNTFRQ_EL0) of the Tegra X1 generic timer.
/// 19.2 MHz -> one 60 Hz frame is 320000 ticks.
inline constexpr std::uint32_t kCntfrqEl0 = 19'200'000;

enum class Interrupt : std::uint32_t {
    None = 0,
    Irq = 1u << 0,
    Fiq = 1u << 1,
};

constexpr std::uint32_t Bit(Interrupt interrupt) noexcept {
    return static_cast<std::uint32_t>(interrupt);
}

enum class MemoryFaultKind : std::uint8_t {
    /// No valid translation for the address at the reported level.
    Translation,
    /// A valid translation exists but access is not permitted (AP bits, UXN/PXN,
    /// or AF == 0).
    Permission,
    Alignment,
    /// The address is outside the range the translation regime covers
    /// (TCR_EL1.T0SZ/T1SZ).
    AddressSize,
};

struct MemoryFault {
    VAddr address = 0;
    MemoryFaultKind kind = MemoryFaultKind::Translation;
    bool is_write = false;
    bool is_fetch = false;
    std::uint8_t size_bytes = 0;
    /// Translation level that faulted (0..3), 0 when not applicable.
    std::uint8_t level = 0;
};

/// A guest SVC (supervisor call) that must be handled by the kernel.
struct SvcCall {
    std::uint32_t swi = 0;
    /// Address of the SVC instruction itself. dynarmic advances PC past the
    /// instruction before invoking CallSVC, so this is reported as PC - 4.
    VAddr pc = 0;
};

enum class UndefinedReason : std::uint8_t {
    UnallocatedEncoding,
    ReservedValue,
    Unpredictable,
    Breakpoint,
    InterpreterFallback,
    Other,
};

struct UndefinedInsn {
    /// Address of the offending instruction itself.
    ///
    /// Note the two dynarmic paths differ in what the guest PC register holds
    /// when this is reported: for exception-raising cases (BRK, unallocated,
    /// reserved, unpredictable, hint) dynarmic does SetPC(PC + 4) first, so the
    /// PC has already advanced past it; for an interpreter fallback it has not.
    VAddr pc = 0;
    UndefinedReason reason = UndefinedReason::Other;
};

}  // namespace zlong::cpu
