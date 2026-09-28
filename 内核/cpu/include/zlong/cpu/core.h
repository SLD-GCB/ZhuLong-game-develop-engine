// 烛龙 (ZhuLong) - one guest core: a dynarmic JIT plus its host thread.
//
// Threading contract:
//   * Only the core's own thread may touch the JIT's guest-visible state
//     (registers, PC, PSTATE) while it is running. From other threads the only
//     legal operations are the ones dynarmic documents as thread-safe:
//     HaltExecution / InvalidateCacheRange / IsExecuting -- plus our own
//     latches (Pause/Resume/AssertInterrupt/Unpark).
//   * Read registers only while the core is stopped (paused, parked, or before
//     Start()).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/halt_reason.h>

#include "zlong/cpu/arm64_interface.h"
#include "zlong/cpu/host.h"
#include "zlong/cpu/memory.h"
#include "zlong/cpu/types.h"

namespace zlong::cpu {

class Cpu;

// Halt reasons raised by this layer. dynarmic's HaltReason is a sticky bitflag
// and Run() only auto-clears CacheInvalidation, so every bit we own has to be
// cleared by us before the next Run(). CacheInvalidation is deliberately not in
// this set: dynarmic consumes it itself.
inline constexpr Dynarmic::HaltReason kHaltUndefined = Dynarmic::HaltReason::UserDefined1;
inline constexpr Dynarmic::HaltReason kHaltSvc = Dynarmic::HaltReason::UserDefined2;
inline constexpr Dynarmic::HaltReason kHaltInterrupt = Dynarmic::HaltReason::UserDefined3;
inline constexpr Dynarmic::HaltReason kHaltPause = Dynarmic::HaltReason::UserDefined4;
inline constexpr Dynarmic::HaltReason kHaltWait = Dynarmic::HaltReason::UserDefined5;
inline constexpr Dynarmic::HaltReason kHaltStop = Dynarmic::HaltReason::UserDefined6;

inline constexpr std::uint32_t kOwnedHaltMask =
    static_cast<std::uint32_t>(Dynarmic::HaltReason::MemoryAbort) |
    static_cast<std::uint32_t>(Dynarmic::HaltReason::UserDefined1) |
    static_cast<std::uint32_t>(Dynarmic::HaltReason::UserDefined2) |
    static_cast<std::uint32_t>(Dynarmic::HaltReason::UserDefined3) |
    static_cast<std::uint32_t>(Dynarmic::HaltReason::UserDefined4) |
    static_cast<std::uint32_t>(Dynarmic::HaltReason::UserDefined5) |
    static_cast<std::uint32_t>(Dynarmic::HaltReason::UserDefined6);

class Core {
public:
    Core(Cpu& cpu, std::size_t index, GuestMemory& memory);
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // --- lifecycle ---------------------------------------------------------
    /// Spawn the host thread. Until this is called the core can be driven
    /// synchronously with RunSynchronously() instead.
    void Start();
    /// Ask the host thread to finish and join it.
    void RequestStop();

    /// Stop the core at the next block boundary and keep it stopped until
    /// Resume(). Idempotent. Safe from any thread.
    void Pause();
    void Resume();

    /// Run one timeslice on the calling thread. Only legal before Start().
    /// Intended for tests and single-step bring-up.
    Dynarmic::HaltReason RunSynchronously(std::uint64_t instruction_budget);

    // --- interrupts and events --------------------------------------------
    void AssertInterrupt(Interrupt interrupt);
    void ClearInterrupt(Interrupt interrupt);

    void SetEventRegister();
    void ClearEventRegister();
    bool EventRegisterSet() const noexcept;

    // --- blocking (called by the kernel/HLE) -------------------------------
    /// Block this core's own thread until Unpark(). Only legal from the core's
    /// own thread and only outside dynarmic's Run() (i.e. from a CpuHost
    /// callback). The other three cores keep running.
    void ParkCurrentThread();
    /// Wake a core blocked in ParkCurrentThread(). Safe from any thread.
    void Unpark();

    // --- observation -------------------------------------------------------
    bool IsExecuting() const;
    bool IsPaused() const;
    bool IsParked() const;
    std::size_t Index() const noexcept { return index_; }

    Dynarmic::A64::Jit& GetJit() noexcept { return jit_; }
    GuestMemory& Memory() const noexcept { return memory_; }
    Cpu& GetCpu() const noexcept { return cpu_; }

    // --- the kernel's window onto the guest registers ------------------------
    //
    // Deliberately not a Jit& : the kernel needs x0..x30, and handing it the
    // frontend's type would tie the kernel to whichever core is underneath.
    // An index above 30 is refused rather than forwarded, so a bad index cannot
    // reach into the frontend's register numbering.

    /// Where a thread begins. The kernel sets these before a core starts, and
    /// gets them without reaching for the frontend type.
    void SetPC(std::uint64_t value) { jit_.SetPC(value); }
    std::uint64_t GetPC() const { return jit_.GetPC(); }
    void SetSP(std::uint64_t value) { jit_.SetSP(value); }
    std::uint64_t GetSP() const { return jit_.GetSP(); }

    /// Read x0..x30. Anything else reads as zero (which is what x31/XZR reads as).
    std::uint64_t ReadGpr(std::size_t index) {
        return index < 31 ? jit_.GetRegister(index) : 0;
    }
    /// Write x0..x30. Anything else is discarded (writes to XZR do nothing).
    void WriteGpr(std::size_t index, std::uint64_t value) {
        if (index < 31) {
            jit_.SetRegister(index, value);
        }
    }

    // --- called by Arm64Interface, on this core's own thread ---------------
    void ReportSvc(const SvcCall& call);
    void ReportMemoryFault(const MemoryFault& fault);
    void ReportUndefined(const UndefinedInsn& insn);
    void AddTicks(std::uint64_t ticks);
    std::uint64_t TicksRemaining() const noexcept { return ticks_remaining_; }

    /// WFE. Returns true if a pending event was consumed (keep running at the
    /// next instruction), false if the core is now waiting (halted).
    bool OnWaitForEvent();
    /// WFI. Always halts the core until an interrupt is asserted.
    void OnWaitForInterrupt();
    /// SEV: broadcast the event to every core.
    void OnSendEvent();
    /// SEVL: set this core's own event register.
    void OnSendEventLocal();
    /// YIELD: hint the host scheduler.
    void OnYield();
    /// BRK / unallocated / reserved / unpredictable: report and halt.
    void OnTrapped(Dynarmic::A64::VAddr pc, UndefinedReason reason);
    /// Instruction fetch fault (MemoryReadCode returned std::nullopt).
    void OnInstructionAbort(Dynarmic::A64::VAddr pc);

private:
    /// Why this core is not executing guest code.
    enum class WaitKind : std::uint8_t { None, Event, Interrupt };

    Dynarmic::A64::UserConfig MakeConfig();
    void ThreadMain();
    bool WaitUntilRunnable();
    void DeliverInterrupts();
    void HandleRunResult(Dynarmic::HaltReason reason);

    Cpu& cpu_;
    const std::size_t index_;
    GuestMemory& memory_;

    // Declaration order is load-bearing: MakeConfig() hands dynarmic the
    // addresses of these members, and dynarmic bakes them into emitted code.
    // Core must therefore be heap-allocated and never moved.
    std::uint64_t tpidr_el0_ = 0;
    std::uint64_t tpidrro_el0_ = 0;
    Arm64Interface callbacks_;
    Dynarmic::A64::Jit jit_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false;
    bool running_ = false;
    bool has_work_ = false;
    bool parked_ = false;
    bool paused_ = false;

    std::atomic<std::uint32_t> pending_interrupts_{0};
    std::atomic<bool> event_register_{false};

    // Own-thread-only state.
    WaitKind wait_kind_ = WaitKind::None;
    std::optional<MemoryFault> pending_fault_;
    std::optional<SvcCall> pending_svc_;
    std::optional<UndefinedInsn> pending_undefined_;
    std::uint64_t ticks_remaining_ = 0;
};

}  // namespace zlong::cpu
