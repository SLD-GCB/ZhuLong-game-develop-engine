# 内核-cpu

# `arm64_interface.h`

`内核/cpu/include/zlong/cpu/arm64_interface.h`

```
烛龙 (ZhuLong) - dynarmic UserCallbacks adapter for one guest core.

This class holds no state of its own: it forwards to the Core. Every memory
callback obeys the no-throw contract documented in memory.h.
```

```cpp
class Core;
class Arm64Interface final : public Dynarmic::A64::UserCallbacks {
    explicit Arm64Interface(Core& core) noexcept : core_(core) {}
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr vaddr) override;
    void MemoryWrite8(Dynarmic::A64::VAddr vaddr, std::uint8_t value) override;
    void MemoryWrite16(Dynarmic::A64::VAddr vaddr, std::uint16_t value) override;
    void MemoryWrite32(Dynarmic::A64::VAddr vaddr, std::uint32_t value) override;
    void MemoryWrite64(Dynarmic::A64::VAddr vaddr, std::uint64_t value) override;
    void MemoryWrite128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value) override;
    // Exclusive stores. These are NOT a fallback for when no global monitor is
    // configured: dynarmic's monitor validates the reservation and then calls
    // these to carry out the store, using the returned bool as the STXR status
    // (true = success). Leaving them at dynarmic's "always false" default makes
    // every STXR fail, i.e. every LDXR/STXR loop spins forever.
    bool MemoryWriteExclusive8(Dynarmic::A64::VAddr vaddr, std::uint8_t value,
                               std::uint8_t expected) override;
    bool MemoryWriteExclusive16(Dynarmic::A64::VAddr vaddr, std::uint16_t value,
                                std::uint16_t expected) override;
    bool MemoryWriteExclusive32(Dynarmic::A64::VAddr vaddr, std::uint32_t value,
                                std::uint32_t expected) override;
    bool MemoryWriteExclusive64(Dynarmic::A64::VAddr vaddr, std::uint64_t value,
                                std::uint64_t expected) override;
    bool MemoryWriteExclusive128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value,
                                 Dynarmic::A64::Vector expected) override;
    void InterpreterFallback(Dynarmic::A64::VAddr pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(Dynarmic::A64::VAddr pc, Dynarmic::A64::Exception exception) override;
    void AddTicks(std::uint64_t ticks) override;
    void WriteBytes(Dynarmic::A64::VAddr vaddr, std::size_t n, std::uint64_t value);
    void ReportFault(Dynarmic::A64::VAddr vaddr, std::size_t n, bool is_write, bool is_fetch);
```

---

# `core.h`

`内核/cpu/include/zlong/cpu/core.h`

```
烛龙 (ZhuLong) - one guest core: a dynarmic JIT plus its host thread.

Threading contract:
* Only the core's own thread may touch the JIT's guest-visible state
(registers, PC, PSTATE) while it is running. From other threads the only
legal operations are the ones dynarmic documents as thread-safe:
HaltExecution / InvalidateCacheRange / IsExecuting -- plus our own
latches (Pause/Resume/AssertInterrupt/Unpark).
* Read registers only while the core is stopped (paused, parked, or before
Start()).
```

```cpp
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
    // --- lifecycle ---------------------------------------------------------
    // Spawn the host thread. Until this is called the core can be driven
    // synchronously with RunSynchronously() instead.
    void Start();
    // Ask the host thread to finish and join it.
    void RequestStop();
    // Stop the core at the next block boundary and keep it stopped until
    // Resume(). Idempotent. Safe from any thread.
    void Pause();
    void Resume();
    // Run one timeslice on the calling thread. Only legal before Start().
    // Intended for tests and single-step bring-up.
    Dynarmic::HaltReason RunSynchronously(std::uint64_t instruction_budget);
    // --- interrupts and events --------------------------------------------
    void AssertInterrupt(Interrupt interrupt);
    void ClearInterrupt(Interrupt interrupt);
    void SetEventRegister();
    void ClearEventRegister();
    bool EventRegisterSet() const noexcept;
    // --- blocking (called by the kernel/HLE) -------------------------------
    // Block this core's own thread until Unpark(). Only legal from the core's
    // own thread and only outside dynarmic's Run() (i.e. from a CpuHost
    // callback). The other three cores keep running.
    void ParkCurrentThread();
    // Wake a core blocked in ParkCurrentThread(). Safe from any thread.
    void Unpark();
    // --- observation -------------------------------------------------------
    bool IsExecuting() const;
    bool IsPaused() const;
    bool IsParked() const;
    Dynarmic::A64::Jit& GetJit() noexcept { return jit_; }
    GuestMemory& Memory() const noexcept { return memory_; }
    Cpu& GetCpu() const noexcept { return cpu_; }
    // Where a thread begins. The kernel sets these before a core starts, and
    // gets them without reaching for the frontend type.
    void SetPC(std::uint64_t value) { jit_.SetPC(value); }
    void SetSP(std::uint64_t value) { jit_.SetSP(value); }
    // Write x0..x30. Anything else is discarded (writes to XZR do nothing).
    void WriteGpr(std::size_t index, std::uint64_t value) {
    // --- called by Arm64Interface, on this core's own thread ---------------
    void ReportSvc(const SvcCall& call);
    void ReportMemoryFault(const MemoryFault& fault);
    void ReportUndefined(const UndefinedInsn& insn);
    void AddTicks(std::uint64_t ticks);
    // WFE. Returns true if a pending event was consumed (keep running at the
    // next instruction), false if the core is now waiting (halted).
    bool OnWaitForEvent();
    // WFI. Always halts the core until an interrupt is asserted.
    void OnWaitForInterrupt();
    // SEV: broadcast the event to every core.
    void OnSendEvent();
    // SEVL: set this core's own event register.
    void OnSendEventLocal();
    // YIELD: hint the host scheduler.
    void OnYield();
    // BRK / unallocated / reserved / unpredictable: report and halt.
    void OnTrapped(Dynarmic::A64::VAddr pc, UndefinedReason reason);
    // Instruction fetch fault (MemoryReadCode returned std::nullopt).
    void OnInstructionAbort(Dynarmic::A64::VAddr pc);
    // Why this core is not executing guest code.
    enum class WaitKind : std::uint8_t { None, Event, Interrupt };
    Dynarmic::A64::UserConfig MakeConfig();
    void ThreadMain();
    bool WaitUntilRunnable();
    void DeliverInterrupts();
    void HandleRunResult(Dynarmic::HaltReason reason);
```

---

# `cpu.h`

`内核/cpu/include/zlong/cpu/cpu.h`

```
烛龙 (ZhuLong) - the CPU layer: four Cortex-A57 cores that cooperate.

The four cores run on four host threads (true parallelism). What makes them
cooperate rather than merely coexist:
* one shared Dynarmic::ExclusiveMonitor(4) + a distinct processor_id per
core, so LDXR/STXR form real cross-core atomic sequences;
* one shared GuestMemory, so one core's stores are immediately visible to
the others (no emulated cache model);
* a shared monotonic CNTPCT_EL0 counter;
* SEV/event broadcast across cores;
* JIT code-cache invalidation broadcast, so self-modifying code written by
one core is observed by all four.
```

```cpp
class Cpu final : public CodeWriteObserver {
    void StartAll();
    void StopAll();
    Core& GetCore(std::size_t index);
    const Core& GetCore(std::size_t index) const;
    static constexpr std::size_t CoreCount() noexcept { return kCoreCount; }
    // CodeWriteObserver: a guest store touched executable memory somewhere, so
    // every core's translations for that range are now stale.
    void OnGuestCodeWrite(VAddr address, std::size_t length) override;
    // CodeWriteObserver: guest memory that page tables live in was modified,
    // so permissions/executability may have changed anywhere. Flushes every
    // core's entire code cache.
    void OnGuestCodeInvalidateAll() override;
    // SEV: set the event register on every core and wake the ones waiting.
    void BroadcastEvent();
    void AdvanceCNTPCT(std::uint64_t ticks) noexcept;
    // Advisory frame-boundary hook: records progress and, if this core has run
    // far ahead of the slowest one, hints the scheduler with a yield. It is
    // deliberately not a barrier -- a hard rendezvous would deadlock as soon
    // as one core parks in WFE or blocks in a syscall.
    void ReportFrameBoundary(std::size_t index);
    Dynarmic::ExclusiveMonitor& ExclusiveMonitorRef() noexcept { return exclusive_monitor_; }
    CpuHost& Host() noexcept { return host_; }
```

---

# `host.h`

`内核/cpu/include/zlong/cpu/host.h`

```
烛龙 (ZhuLong) - the kernel/HLE seam of the CPU layer.

This is the ONLY place the kernel plugs into the CPU. The CPU layer contains
no syscall numbers, no HLE, no guest exception model: it reports events and
the kernel decides what they mean.
```

```cpp
class Core;
class CpuHost {
    virtual ~CpuHost() = default;
    // A guest core executed SVC. Called on that core's own thread, and only
    // while the core is NOT inside dynarmic's Run() -- so a blocking syscall
    // handler may call Core::ParkCurrentThread() safely.
    //
    // Convention: dynarmic has already advanced PC past the SVC. The handler
    // must not modify PC except for guest exception-return sequences; on
    // return the core resumes from the current JIT state.
    virtual void OnSvc(Core& core, const SvcCall& call) = 0;
    // Deliver a pending IRQ/FIQ. Called on the core's own thread between
    // Run() calls; the kernel performs the guest exception entry
    // (ELR_EL1/SPSR_EL1/VBAR, PSTATE masking) via Core::GetJit().
    virtual void OnInterrupt(Core& core, Interrupt interrupt) = 0;
    // A guest data access hit unmapped/protected memory. The core has already
    // been halted cleanly; the kernel decides whether this becomes a guest
    // data abort or kills the emulation.
    virtual void OnMemoryFault(Core& /*core*/, const MemoryFault& /*fault*/) {}
    // A guest instruction could not be translated (unallocated encoding,
    // reserved value, BRK, or a dynarmic interpreter fallback).
    virtual void OnUndefinedInstruction(Core& /*core*/, const UndefinedInsn& /*insn*/) {}
    // The core's per-frame cycle budget ran out. Advisory scheduling hook;
    // the default does nothing.
    virtual void OnTimesliceExpired(Core& /*core*/) {}
```

---

# `memory.h`

`内核/cpu/include/zlong/cpu/memory.h`

```
烛龙 (ZhuLong) - guest memory bus contract for the CPU layer.

The concrete implementation lives in 内核/RAM (physical memory + ARMv8-A MMU,
behind its own cache so straddling and permissions are handled there).

Contract, and it matters:
* Addresses are guest VIRTUAL addresses. dynarmic performs no translation.
* All accessors are little-endian.
* Failures are reported by return value (std::nullopt / false). They must
NEVER throw: the CPU calls them from underneath JIT-generated code that
has no unwind information, so a propagating C++ exception is undefined
behaviour.
```

```cpp
// Details of a failed access, recovered by the CPU layer so it can report a
// real fault reason instead of a placeholder. The accessors themselves only
// carry success/failure because they are the hot path.
struct FaultInfo {
// Result of asking the memory layer to interpret a guest system instruction
// that dynarmic could not translate (it reaches UserCallbacks::InterpreterFallback).
struct SystemInsnResult {
    enum class Kind : std::uint8_t {
// Implemented by whoever owns guest code memory. The CPU layer registers
// itself here so that a guest store into an executable region invalidates the
// JIT code caches (self-modifying code). Only executable regions should be
// reported -- reporting every store makes invalidation the dominant cost.
class CodeWriteObserver {
    virtual ~CodeWriteObserver() = default;
    virtual void OnGuestCodeWrite(VAddr address, std::size_t length) = 0;
    // The whole address space is suspect: flush every JIT code cache. Used
    // when guest memory that page tables themselves live in was modified --
    // executable/permission bits may have changed anywhere.
    virtual void OnGuestCodeInvalidateAll() {}
class GuestMemory {
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
    // Instruction fetch, 4-byte aligned. std::nullopt means "no executable
    // memory here" and produces a guest instruction abort.
    virtual std::optional<std::uint32_t> TryFetch32(VAddr address) = 0;
    // Explain the access that just failed, so the CPU layer can attribute a
    // real reason (translation vs permission, and the level). Defaulted: an
    // implementation that has nothing to add reports a translation fault.
    //
    // Implementations must keep this race-free across the four cores. Storing
    // the detail in thread-local state is the intended approach: the failing
    // access and this query happen on the same host thread.
    virtual FaultInfo DescribeFault(VAddr /*address*/, bool /*is_write*/, bool /*is_fetch*/) const {
    // A guest instruction dynarmic left untranslated; see SystemInsnResult.
    // `core` identifies which guest core executed it (per-core state such as
    // MPIDR_EL1 depends on it). `instruction` is the raw 32-bit encoding and
    // `rt_value` is the current value of its Rt register. Defaulted: nothing
    // implemented, so the CPU layer keeps treating it as an unsupported trap.
    virtual SystemInsnResult HandleSystemInstruction(std::size_t /*core*/, VAddr /*pc*/,
                                                      std::uint32_t /*instruction*/,
                                                      std::uint64_t /*rt_value*/) {
    void SetCodeWriteObserver(CodeWriteObserver* observer) noexcept { observer_ = observer; }
    CodeWriteObserver* GetCodeWriteObserver() const noexcept { return observer_; }
    // Concrete implementations call this from their write path when the
    // touched range lies in executable guest memory.
    void NotifyCodeWrite(VAddr address, std::size_t length) {
    // Concrete implementations call this when the touched range may be page
    // table memory, i.e. when an executable/permission bit may have changed.
    void NotifyCodeInvalidateAll() {
```

---

# `types.h`

`内核/cpu/include/zlong/cpu/types.h`

```
烛龙 (ZhuLong) - CPU layer common types.

Deliberately dependency-free: nothing here includes dynarmic, so the rest of
the kernel can talk about CPU state without pulling in the JIT.
```

```cpp
using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;  // {low64, high64}
// NVIDIA Tegra X1: 4x ARM Cortex-A57.
inline constexpr std::size_t kCoreCount = 4;
// Generic-timer frequency (CNTFRQ_EL0) of the Tegra X1 generic timer.
// 19.2 MHz -> one 60 Hz frame is 320000 ticks.
inline constexpr std::uint32_t kCntfrqEl0 = 19'200'000;
enum class Interrupt : std::uint32_t {
constexpr std::uint32_t Bit(Interrupt interrupt) noexcept {
enum class MemoryFaultKind : std::uint8_t {
struct MemoryFault {
// A guest SVC (supervisor call) that must be handled by the kernel.
struct SvcCall {
enum class UndefinedReason : std::uint8_t {
struct UndefinedInsn {
```

---
