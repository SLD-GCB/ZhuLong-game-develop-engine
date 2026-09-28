// 烛龙 (ZhuLong) - guest memory bus contract for the CPU layer.
//
// The concrete implementation lives in 内核/RAM (physical memory + ARMv8-A MMU,
// behind its own cache so straddling and permissions are handled there).
//
// Contract, and it matters:
//   * Addresses are guest VIRTUAL addresses. dynarmic performs no translation.
//   * All accessors are little-endian.
//   * Failures are reported by return value (std::nullopt / false). They must
//     NEVER throw: the CPU calls them from underneath JIT-generated code that
//     has no unwind information, so a propagating C++ exception is undefined
//     behaviour.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "zlong/cpu/types.h"

namespace zlong::cpu {

/// Details of a failed access, recovered by the CPU layer so it can report a
/// real fault reason instead of a placeholder. The accessors themselves only
/// carry success/failure because they are the hot path.
struct FaultInfo {
    MemoryFaultKind kind = MemoryFaultKind::Translation;
    /// Translation level that produced the fault (0..3), 0 when not applicable.
    std::uint8_t level = 0;
};

/// Result of asking the memory layer to interpret a guest system instruction
/// that dynarmic could not translate (it reaches UserCallbacks::InterpreterFallback).
struct SystemInsnResult {
    enum class Kind : std::uint8_t {
        /// Not a system instruction we implement: caller keeps its old behaviour.
        Unhandled,
        /// Consumed. Caller must advance the guest PC by 4 and resume.
        Handled,
    };

    Kind kind = Kind::Unhandled;
    /// GPR index to write back for an MRS, or 0xFF for "no write-back".
    std::uint8_t write_rt = 0xFF;
    std::uint64_t write_value = 0;
};

/// Implemented by whoever owns guest code memory. The CPU layer registers
/// itself here so that a guest store into an executable region invalidates the
/// JIT code caches (self-modifying code). Only executable regions should be
/// reported -- reporting every store makes invalidation the dominant cost.
class CodeWriteObserver {
public:
    virtual ~CodeWriteObserver() = default;
    virtual void OnGuestCodeWrite(VAddr address, std::size_t length) = 0;

    /// The whole address space is suspect: flush every JIT code cache. Used
    /// when guest memory that page tables themselves live in was modified --
    /// executable/permission bits may have changed anywhere.
    virtual void OnGuestCodeInvalidateAll() {}
};

class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    virtual std::optional<std::uint8_t> TryRead8(VAddr address) = 0;
    virtual std::optional<std::uint16_t> TryRead16(VAddr address) = 0;
    virtual std::optional<std::uint32_t> TryRead32(VAddr address) = 0;
    virtual std::optional<std::uint64_t> TryRead64(VAddr address) = 0;
    virtual std::optional<Vector> TryRead128(VAddr address) = 0;

    virtual bool TryWrite8(VAddr address, std::uint8_t value) = 0;
    virtual bool TryWrite16(VAddr address, std::uint16_t value) = 0;
    virtual bool TryWrite32(VAddr address, std::uint32_t value) = 0;
    virtual bool TryWrite64(VAddr address, std::uint64_t value) = 0;
    virtual bool TryWrite128(VAddr address, Vector value) = 0;

    /// Instruction fetch, 4-byte aligned. std::nullopt means "no executable
    /// memory here" and produces a guest instruction abort.
    virtual std::optional<std::uint32_t> TryFetch32(VAddr address) = 0;

    /// Explain the access that just failed, so the CPU layer can attribute a
    /// real reason (translation vs permission, and the level). Defaulted: an
    /// implementation that has nothing to add reports a translation fault.
    ///
    /// Implementations must keep this race-free across the four cores. Storing
    /// the detail in thread-local state is the intended approach: the failing
    /// access and this query happen on the same host thread.
    virtual FaultInfo DescribeFault(VAddr /*address*/, bool /*is_write*/, bool /*is_fetch*/) const {
        return {};
    }

    /// A guest instruction dynarmic left untranslated; see SystemInsnResult.
    /// `core` identifies which guest core executed it (per-core state such as
    /// MPIDR_EL1 depends on it). `instruction` is the raw 32-bit encoding and
    /// `rt_value` is the current value of its Rt register. Defaulted: nothing
    /// implemented, so the CPU layer keeps treating it as an unsupported trap.
    virtual SystemInsnResult HandleSystemInstruction(std::size_t /*core*/, VAddr /*pc*/,
                                                      std::uint32_t /*instruction*/,
                                                      std::uint64_t /*rt_value*/) {
        return {};
    }

    void SetCodeWriteObserver(CodeWriteObserver* observer) noexcept { observer_ = observer; }
    CodeWriteObserver* GetCodeWriteObserver() const noexcept { return observer_; }

protected:
    /// Concrete implementations call this from their write path when the
    /// touched range lies in executable guest memory.
    void NotifyCodeWrite(VAddr address, std::size_t length) {
        if (observer_ != nullptr) {
            observer_->OnGuestCodeWrite(address, length);
        }
    }

    /// Concrete implementations call this when the touched range may be page
    /// table memory, i.e. when an executable/permission bit may have changed.
    void NotifyCodeInvalidateAll() {
        if (observer_ != nullptr) {
            observer_->OnGuestCodeInvalidateAll();
        }
    }

private:
    CodeWriteObserver* observer_ = nullptr;
};

}  // namespace zlong::cpu
