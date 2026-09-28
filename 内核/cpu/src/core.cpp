#include "zlong/cpu/core.h"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

#include "zlong/cpu/cpu.h"

namespace zlong::cpu {

Core::Core(Cpu& cpu, std::size_t index, GuestMemory& memory)
    : cpu_(cpu),
      index_(index),
      memory_(memory),
      callbacks_(*this),
      jit_(MakeConfig()) {}

Core::~Core() {
    RequestStop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

Dynarmic::A64::UserConfig Core::MakeConfig() {
    // These pointers are baked into emitted code by dynarmic, which is why the
    // members they refer to are declared before jit_ and this Core is never
    // moved.
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &callbacks_;
    config.processor_id = index_;
    config.global_monitor = &cpu_.ExclusiveMonitorRef();
    config.enable_cycle_counting = true;
    config.optimizations = Dynarmic::all_safe_optimizations;
    config.unsafe_optimizations = false;
    config.cntfrq_el0 = cpu_.CntfrqEl0();
    config.tpidr_el0 = &tpidr_el0_;
    config.tpidrro_el0 = &tpidrro_el0_;
    config.code_cache_size = 32 * 1024 * 1024;  // dynarmic's floor is ~8 MiB
    return config;
}

// ---------------------------------------------------------------- lifecycle --

void Core::Start() {
    if (started_) {
        throw std::logic_error("Core::Start() called twice");
    }
    started_ = true;
    {
        std::lock_guard lock{mutex_};
        running_ = true;
    }
    thread_ = std::thread([this] { ThreadMain(); });
}

void Core::RequestStop() {
    {
        std::lock_guard lock{mutex_};
        running_ = false;
    }
    jit_.HaltExecution(kHaltStop);
    cv_.notify_all();
}

void Core::Pause() {
    {
        std::lock_guard lock{mutex_};
        paused_ = true;
    }
    jit_.HaltExecution(kHaltPause);
    cv_.notify_all();
}

void Core::Resume() {
    {
        std::lock_guard lock{mutex_};
        paused_ = false;
    }
    cv_.notify_all();
}

Dynarmic::HaltReason Core::RunSynchronously(std::uint64_t instruction_budget) {
    if (started_) {
        throw std::logic_error("Core::RunSynchronously() requires a core that has not been started");
    }
    pending_fault_.reset();
    pending_svc_.reset();
    pending_undefined_.reset();
    wait_kind_ = WaitKind::None;
    ticks_remaining_ = instruction_budget;
    jit_.ClearHalt(static_cast<Dynarmic::HaltReason>(kOwnedHaltMask));
    const auto reason = jit_.Run();
    HandleRunResult(reason);
    return reason;
}

// ------------------------------------------------------------ interrupts ----

void Core::AssertInterrupt(Interrupt interrupt) {
    if (interrupt == Interrupt::None) {
        return;
    }
    pending_interrupts_.fetch_or(Bit(interrupt), std::memory_order_acq_rel);
    if (jit_.IsExecuting()) {
        jit_.HaltExecution(kHaltInterrupt);
    }
    // Wakes a core blocked in WFI; the waiter re-checks the latch under the
    // lock, so a notify that arrives before it sleeps cannot be lost.
    cv_.notify_all();
}

void Core::ClearInterrupt(Interrupt interrupt) {
    pending_interrupts_.fetch_and(~Bit(interrupt), std::memory_order_acq_rel);
}

// ----------------------------------------------------------------- events ---

void Core::SetEventRegister() {
    event_register_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void Core::ClearEventRegister() {
    event_register_.store(false, std::memory_order_release);
}

bool Core::EventRegisterSet() const noexcept {
    return event_register_.load(std::memory_order_acquire);
}

// --------------------------------------------------------------- blocking ---

void Core::ParkCurrentThread() {
    std::unique_lock lock{mutex_};
    parked_ = true;
    cv_.wait(lock, [this] { return !running_ || has_work_; });
    has_work_ = false;
    parked_ = false;
}

void Core::Unpark() {
    {
        std::lock_guard lock{mutex_};
        has_work_ = true;
    }
    cv_.notify_all();
}

// ------------------------------------------------------------ observation ---

bool Core::IsExecuting() const {
    return jit_.IsExecuting();
}

bool Core::IsPaused() const {
    std::lock_guard lock{mutex_};
    return paused_;
}

bool Core::IsParked() const {
    std::lock_guard lock{mutex_};
    return parked_ || paused_ || wait_kind_ != WaitKind::None;
}

// ------------------------------------------- reporting (own thread only) ----

void Core::ReportSvc(const SvcCall& call) {
    pending_svc_ = call;
}

void Core::ReportMemoryFault(const MemoryFault& fault) {
    // Keep the first fault: a multi-element access reports once per element,
    // and the earliest one is the architecturally relevant address.
    if (!pending_fault_) {
        pending_fault_ = fault;
    }
}

void Core::ReportUndefined(const UndefinedInsn& insn) {
    if (!pending_undefined_) {
        pending_undefined_ = insn;
    }
}

void Core::AddTicks(std::uint64_t ticks) {
    ticks_remaining_ = (ticks >= ticks_remaining_) ? 0 : ticks_remaining_ - ticks;
    cpu_.AdvanceCNTPCT(ticks);
}

// ------------------------------------------------------- hint instruction ---

bool Core::OnWaitForEvent() {
    if (event_register_.exchange(false, std::memory_order_acq_rel)) {
        return true;  // an event was already pending: consume it and carry on
    }
    {
        std::lock_guard lock{mutex_};
        wait_kind_ = WaitKind::Event;
    }
    jit_.HaltExecution(kHaltWait);
    return false;
}

void Core::OnWaitForInterrupt() {
    {
        std::lock_guard lock{mutex_};
        wait_kind_ = WaitKind::Interrupt;
    }
    jit_.HaltExecution(kHaltWait);
}

void Core::OnSendEvent() {
    cpu_.BroadcastEvent();
}

void Core::OnSendEventLocal() {
    SetEventRegister();
}

void Core::OnYield() {
    std::this_thread::yield();
}

void Core::OnTrapped(Dynarmic::A64::VAddr pc, UndefinedReason reason) {
    UndefinedInsn insn;
    insn.pc = pc;
    insn.reason = reason;
    ReportUndefined(insn);
    jit_.HaltExecution(kHaltUndefined);
}

void Core::OnInstructionAbort(Dynarmic::A64::VAddr pc) {
    // Instruction aborts originate inside dynarmic (MemoryReadCode returned
    // nullopt -> Exception::NoExecuteFault), so Arm64Interface never sees them.
    // This is the next place with both the address and the memory layer.
    MemoryFault fault;
    fault.address = pc;
    const FaultInfo info = memory_.DescribeFault(pc, /*is_write=*/false, /*is_fetch=*/true);
    fault.kind = info.kind;
    fault.level = info.level;
    fault.is_write = false;
    fault.is_fetch = true;
    fault.size_bytes = 4;
    ReportMemoryFault(fault);
    jit_.HaltExecution(Dynarmic::HaltReason::MemoryAbort);
}

// --------------------------------------------------------------- run loop ---

void Core::ThreadMain() {
    for (;;) {
        if (!WaitUntilRunnable()) {
            return;
        }
        // Clear only the bits this layer raises. dynarmic owns
        // CacheInvalidation and consumes it itself; clearing it here would
        // drop a pending invalidation. A request that races in after this
        // point re-sets its bit and is picked up by the next Run().
        jit_.ClearHalt(static_cast<Dynarmic::HaltReason>(kOwnedHaltMask));
        DeliverInterrupts();
        ticks_remaining_ = cpu_.TicksPerFrame();
        HandleRunResult(jit_.Run());
    }
}

bool Core::WaitUntilRunnable() {
    std::unique_lock lock{mutex_};
    for (;;) {
        if (!running_) {
            return false;
        }
        if (paused_) {
            cv_.wait(lock, [this] { return !running_ || !paused_; });
            continue;
        }
        if (wait_kind_ != WaitKind::None) {
            const WaitKind kind = wait_kind_;
            cv_.wait(lock, [this, kind] {
                if (!running_) {
                    return true;
                }
                if (pending_interrupts_.load(std::memory_order_acquire) != 0) {
                    return true;
                }
                return kind == WaitKind::Event && event_register_.load(std::memory_order_acquire);
            });
            if (!running_) {
                return false;
            }
            if (pending_interrupts_.load(std::memory_order_acquire) == 0) {
                // Woken by the event rather than an interrupt: WFE consumes it.
                event_register_.store(false, std::memory_order_acq_rel);
            }
            wait_kind_ = WaitKind::None;
            continue;
        }
        if (parked_) {
            cv_.wait(lock, [this] { return !running_ || has_work_; });
            has_work_ = false;
            continue;
        }
        return true;
    }
}

void Core::DeliverInterrupts() {
    const std::uint32_t bits = pending_interrupts_.exchange(0, std::memory_order_acq_rel);
    if (bits == 0) {
        return;
    }
    if ((bits & Bit(Interrupt::Fiq)) != 0) {
        if ((bits & Bit(Interrupt::Irq)) != 0) {
            // Deliver one exception entry at a time; keep IRQ pending.
            pending_interrupts_.fetch_or(Bit(Interrupt::Irq), std::memory_order_acq_rel);
        }
        cpu_.Host().OnInterrupt(*this, Interrupt::Fiq);
        return;
    }
    cpu_.Host().OnInterrupt(*this, Interrupt::Irq);
}

void Core::HandleRunResult(Dynarmic::HaltReason reason) {
    if (pending_fault_) {
        const MemoryFault fault = *pending_fault_;
        pending_fault_.reset();
        cpu_.Host().OnMemoryFault(*this, fault);
    }
    if (pending_undefined_) {
        const UndefinedInsn insn = *pending_undefined_;
        pending_undefined_.reset();
        cpu_.Host().OnUndefinedInstruction(*this, insn);
    }
    if (pending_svc_) {
        // Called on this core's own thread and outside Run(), so a blocking
        // syscall handler may safely call ParkCurrentThread() from here.
        const SvcCall call = *pending_svc_;
        pending_svc_.reset();
        cpu_.Host().OnSvc(*this, call);
    }
    if (!reason) {
        // A falsy HaltReason means the cycle budget ran out: the natural
        // scheduling point. This is what keeps a spinning guest from pinning a
        // host thread.
        cpu_.ReportFrameBoundary(index_);
        cpu_.Host().OnTimesliceExpired(*this);
    }
}

}  // namespace zlong::cpu
