// 烛龙 (ZhuLong) - the kernel: the CpuHost the CPU layer calls into.
//
// The CPU layer holds no syscall numbers, no HLE and no guest exception model:
// it reports events and this decides what they mean. OnSvc is the front door.
//
// OnInterrupt is the other one, and it is honest about being unfinished: guest
// exception entry is not implemented, so an interrupt does not reach the guest.
// That gap is counted rather than hidden -- see the note on the implementation.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "zlong/cpu/host.h"
#include "zlong/gpu/gpu_host.h"
#include "zlong/ram/ram.h"
#include "zlong/service/address_space.h"
#include "zlong/service/process.h"
#include "zlong/service/registry.h"
#include "zlong/service/svc.h"
#include "zlong/service/thread.h"

namespace zlong::gpu {
class Gpu;
}

namespace zlong::audio {
class Sink;
}

namespace zlong::service {

class NvDrvService;
class AudOutService;
class AudRenService;

class Kernel final : public cpu::CpuHost {
public:
    /// The RAM layer is needed for the address space: the memory syscalls manage
    /// it, and boot maps the entry module through it.
    explicit Kernel(ram::Ram& ram);

    ram::Ram& ram() noexcept { return ram_; }
    KAddressSpace& address_space() noexcept { return address_space_; }
    const KAddressSpace& address_space() const noexcept { return address_space_; }

    SyscallTable& syscalls() noexcept { return syscalls_; }
    const SyscallTable& syscalls() const noexcept { return syscalls_; }

    void OnSvc(cpu::Core& core, const cpu::SvcCall& call) override;
    void OnInterrupt(cpu::Core& core, cpu::Interrupt interrupt) override;

    /// Interrupts that arrived at the host. Guest exception entry is not
    /// implemented yet, so none of these reached the guest -- this is how that
    /// stays visible instead of looking like it worked.
    std::size_t interrupts_seen() const noexcept {
        return interrupts_seen_.load(std::memory_order_relaxed);
    }
    std::uint32_t last_interrupt() const noexcept {
        return last_interrupt_.load(std::memory_order_relaxed);
    }

    /// The most recent debug string a guest asked to be printed, and how many it
    /// has asked for. Bounded, because the guest chooses the length.
    std::string last_debug_output() const;
    std::size_t debug_strings() const noexcept {
        return debug_strings_.load(std::memory_order_relaxed);
    }

    /// The reason code the last Break carried, and how many there were.
    std::uint64_t last_break_reason() const noexcept {
        return last_break_reason_.load(std::memory_order_relaxed);
    }
    std::size_t breaks() const noexcept { return breaks_.load(std::memory_order_relaxed); }

    // --- the waiter bridge ---------------------------------------------------
    //
    // A waitable object reports *which* waiters a signal released. Turning that
    // into a wake is this layer's job: it is the only place that knows which
    // core a thread runs on.
    //
    // The registry BORROWS the thread: the owning process keeps it alive and has
    // to unbind before it dies. Holding a reference here would make a thread
    // outlive its own process, which is a worse lie than a documented contract.

    /// Bind a thread to the core it runs on. False when the waiter id is already
    /// bound to a different thread.
    bool BindThread(KThread& thread, cpu::Core& core);
    bool UnbindThread(WaiterId waiter);
    KThread* FindThread(WaiterId waiter) const;

    /// The thread bound to this core, or nullptr. A scan, because there are a
    /// handful of threads and a second index would be one more thing to keep
    /// consistent.
    KThread* ThreadOn(cpu::Core& core) const;

    /// Wake whoever `waiter` names. False when the id is unknown or the thread
    /// has no core -- a waiter that cannot be reached is a bug, not a no-op, so
    /// it is reported rather than swallowed.
    bool Wake(WaiterId waiter);

    /// Wake everyone a signal released. Called AFTER the object's lock is
    /// dropped, which is what keeps the order object.mutex -> core.mutex.
    std::size_t WakeAll(const std::vector<WaiterId>& waiters);

    /// Threads that could not be woken since the last Reset.
    std::size_t stranded_wakes() const noexcept {
        return stranded_wakes_.load(std::memory_order_relaxed);
    }
    void ResetStrandedWakes() noexcept { stranded_wakes_.store(0, std::memory_order_relaxed); }

    /// The service set. Populated before boot: the kernel does not own which
    /// services exist, only that there is one place they are found.
    ServiceRegistry& services() noexcept { return services_; }
    const ServiceRegistry& services() const noexcept { return services_; }

    /// Wire the GPU submission path: everything nvdrv submits goes here, and what
    /// the GPU reports back goes to the Gpu's own host. Until this is called,
    /// nvdrv exists and refuses every submission with a reason.
    void attach_gpu(gpu::Gpu& gpu) noexcept;

    /// The wired GPU, or nullptr.
    gpu::Gpu* gpu() const noexcept;

    /// Wire the audio device: everything audout's guest hands over, and everything
    /// audren mixes, goes here. Both services get it, so a game is heard through the same
    /// sink whether it appends finished PCM or registers voices. Until this is called,
    /// both refuse with a reason, so a guest is told nothing took its audio.
    void attach_audio(audio::Sink& sink) noexcept;

    /// The wired device, or nullptr.
    audio::Sink* audio_sink() const noexcept;

    /// Hand out a session id, the same way thread ids are handed out.
    std::uint64_t AllocateSessionId() noexcept {
        return next_session_id_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Hand out a thread id. The kernel owns the numbering so that a tid is
    /// never reused while a stale waiter could still name it.
    WaiterId AllocateTid() noexcept {
        return next_tid_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Start a thread the guest created, on the core it was bound to. False when
    /// the handle names nothing, is not a thread, is already started, or the
    /// core is gone. Public for the same reason as SignalEventHandle.
    bool StartThreadHandle(Handle handle);

    /// Program a thread's recorded entry state onto its core -- PC, SP and the
    /// first argument register -- and mark it ready. Does NOT start the core: a
    /// host decides when cores run, and a test drives one synchronously from the
    /// entry point instead. Boot readies a main thread this way.
    ///
    /// False when the thread has already left Created, or has no core.
    bool ReadyThread(KThread& thread);

    /// Stop the calling thread: mark it finished and stop its core. False when
    /// no thread is bound to that core.
    bool ExitCurrentThread(cpu::Core& core);

    /// Signal the event a handle names and wake whoever it released. Public
    /// because the same pair is needed outside a syscall -- a test now, a device
    /// later -- and duplicating it would be two places to get the lock order
    /// wrong. False when the handle names nothing or is not an event.
    bool SignalEventHandle(Handle handle);

    /// The process whose handle table syscalls resolve against. Set at boot.
    void set_process(KProcess* process) noexcept { process_ = process; }
    KProcess* process() const noexcept { return process_; }

private:
    ram::Ram& ram_;
    KAddressSpace address_space_;
    SyscallTable syscalls_;
    KProcess* process_ = nullptr;

    mutable std::mutex threads_mutex_;
    std::map<WaiterId, KThread*> threads_;
    std::atomic<std::size_t> stranded_wakes_{0};
    std::atomic<WaiterId> next_tid_{1};
    ServiceRegistry services_;
    std::atomic<std::uint64_t> next_session_id_{1};

    /// The nvdrv service, held directly so attach_gpu() can reach it without a
    /// downcast. Owned by the registry, like every other service.
    NvDrvService* nvdrv_ = nullptr;

    /// The audout service, held for the same reason.
    AudOutService* audout_ = nullptr;

    /// The audren service, held for the same reason -- attach_audio has to reach both.
    AudRenService* audren_ = nullptr;

    std::atomic<std::size_t> interrupts_seen_{0};
    std::atomic<std::uint32_t> last_interrupt_{0};

    std::atomic<std::size_t> debug_strings_{0};
    std::atomic<std::uint64_t> last_break_reason_{0};
    std::atomic<std::size_t> breaks_{0};
    mutable std::mutex debug_mutex_;
    std::string last_debug_output_;
};

}  // namespace zlong::service
