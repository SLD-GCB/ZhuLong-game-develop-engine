// 烛龙 (ZhuLong) - system registers that dynarmic's A64 frontend does not
// implement.
//
// dynarmic implements MRS/MSR for only nine registers (CNTFRQ_EL0, CNTPCT_EL0,
// CTR_EL0, DCZID_EL0, FPCR, FPSR, NZCV, TPIDR_EL0, TPIDRRO_EL0). Everything
// else lands in UserCallbacks::InterpreterFallback, which is where the memory
// layer picks these up. The encodings below were verified by assembling the
// corresponding mnemonics.
//
// Scope note: only registers that belong to *memory management* live here. The
// exception-state registers (ESR_EL1, FAR_EL1, VBAR_EL1, ELR_EL1, SPSR_EL1)
// belong to the kernel's exception model and are deliberately left unhandled --
// under HLE the guest's EL1 kernel code does not run.

#pragma once

#include <atomic>
#include <cstdint>

namespace zlong::ram {

/// Packed (op1, CRn, CRm, op2) key for a system register.
constexpr std::uint32_t SystemRegKey(std::uint32_t op1, std::uint32_t crn, std::uint32_t crm,
                                     std::uint32_t op2) noexcept {
    return (op1 << 12) | (crn << 8) | (crm << 4) | op2;
}

class SystemRegisters {
public:
    // Cortex-A57 MIDR_EL1: implementer 0x41 (ARM), variant 0, arch 0xF,
    // part 0xD07, revision 0.
    static constexpr std::uint64_t kMidrEl1 = 0x410F'D070ULL;

    // --- translation control (shared: one guest address space under HLE) ----

    std::uint64_t ttbr0_el1() const noexcept { return ttbr0_el1_.load(std::memory_order_acquire); }
    std::uint64_t ttbr1_el1() const noexcept { return ttbr1_el1_.load(std::memory_order_acquire); }
    std::uint64_t tcr_el1() const noexcept { return tcr_el1_.load(std::memory_order_acquire); }
    std::uint64_t mair_el1() const noexcept { return mair_el1_.load(std::memory_order_acquire); }
    std::uint64_t sctlr_el1() const noexcept { return sctlr_el1_.load(std::memory_order_acquire); }
    std::uint64_t cpacr_el1() const noexcept { return cpacr_el1_.load(std::memory_order_acquire); }

    void set_ttbr0_el1(std::uint64_t value) noexcept {
        ttbr0_el1_.store(value, std::memory_order_release);
    }
    void set_ttbr1_el1(std::uint64_t value) noexcept {
        ttbr1_el1_.store(value, std::memory_order_release);
    }
    void set_tcr_el1(std::uint64_t value) noexcept {
        tcr_el1_.store(value, std::memory_order_release);
    }
    void set_mair_el1(std::uint64_t value) noexcept {
        mair_el1_.store(value, std::memory_order_release);
    }
    void set_sctlr_el1(std::uint64_t value) noexcept {
        sctlr_el1_.store(value, std::memory_order_release);
    }
    void set_cpacr_el1(std::uint64_t value) noexcept {
        cpacr_el1_.store(value, std::memory_order_release);
    }

    /// SCTLR_EL1.M: when clear, translation is off and VA == PA.
    bool mmu_enabled() const noexcept {
        return (sctlr_el1() & 1ULL) != 0;
    }

    // --- fields ------------------------------------------------------------

    /// TCR_EL1.T0SZ / T1SZ: the VA size of each TTBR regime is 2^(64 - TnSZ).
    std::uint32_t t0sz() const noexcept { return static_cast<std::uint32_t>(tcr_el1() & 0x3F); }
    std::uint32_t t1sz() const noexcept {
        return static_cast<std::uint32_t>((tcr_el1() >> 16) & 0x3F);
    }

private:
    std::atomic<std::uint64_t> ttbr0_el1_{0};
    std::atomic<std::uint64_t> ttbr1_el1_{0};
    std::atomic<std::uint64_t> tcr_el1_{0};
    std::atomic<std::uint64_t> mair_el1_{0};
    std::atomic<std::uint64_t> sctlr_el1_{0};
    std::atomic<std::uint64_t> cpacr_el1_{0};
};

}  // namespace zlong::ram
