// 烛龙 (ZhuLong) - the CPU layer: four Cortex-A57 cores that cooperate.
//
// The four cores run on four host threads (true parallelism). What makes them
// cooperate rather than merely coexist:
//   * one shared Dynarmic::ExclusiveMonitor(4) + a distinct processor_id per
//     core, so LDXR/STXR form real cross-core atomic sequences;
//   * one shared GuestMemory, so one core's stores are immediately visible to
//     the others (no emulated cache model);
//   * a shared monotonic CNTPCT_EL0 counter;
//   * SEV/event broadcast across cores;
//   * JIT code-cache invalidation broadcast, so self-modifying code written by
//     one core is observed by all four.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <dynarmic/interface/exclusive_monitor.h>

#include "zlong/cpu/core.h"
#include "zlong/cpu/host.h"
#include "zlong/cpu/memory.h"
#include "zlong/cpu/types.h"

namespace zlong::cpu {

class Cpu final : public CodeWriteObserver {
public:
    Cpu(GuestMemory& memory, CpuHost& host);
    ~Cpu() override;

    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    void StartAll();
    void StopAll();

    Core& GetCore(std::size_t index);
    const Core& GetCore(std::size_t index) const;

    static constexpr std::size_t CoreCount() noexcept { return kCoreCount; }

    /// CodeWriteObserver: a guest store touched executable memory somewhere, so
    /// every core's translations for that range are now stale.
    void OnGuestCodeWrite(VAddr address, std::size_t length) override;

    /// CodeWriteObserver: guest memory that page tables live in was modified,
    /// so permissions/executability may have changed anywhere. Flushes every
    /// core's entire code cache.
    void OnGuestCodeInvalidateAll() override;

    /// SEV: set the event register on every core and wake the ones waiting.
    void BroadcastEvent();

    /// Shared monotonic CNTPCT_EL0.
    std::uint64_t GetCNTPCT() const noexcept;
    void AdvanceCNTPCT(std::uint64_t ticks) noexcept;

    /// Cycle budget granted to a core per Run() so a spinning guest cannot
    /// hold a host thread forever.
    std::uint64_t TicksPerFrame() const noexcept;

    std::uint32_t CntfrqEl0() const noexcept { return cntfrq_el0_; }

    /// Advisory frame-boundary hook: records progress and, if this core has run
    /// far ahead of the slowest one, hints the scheduler with a yield. It is
    /// deliberately not a barrier -- a hard rendezvous would deadlock as soon
    /// as one core parks in WFE or blocks in a syscall.
    void ReportFrameBoundary(std::size_t index);

    Dynarmic::ExclusiveMonitor& ExclusiveMonitorRef() noexcept { return exclusive_monitor_; }
    CpuHost& Host() noexcept { return host_; }

private:
    GuestMemory& memory_;
    CpuHost& host_;

    // Declared before cores_: the cores' JIT configs point at this monitor.
    Dynarmic::ExclusiveMonitor exclusive_monitor_{kCoreCount};
    std::array<std::unique_ptr<Core>, kCoreCount> cores_;

    std::atomic<std::uint64_t> cntpct_{0};
    std::array<std::atomic<std::uint64_t>, kCoreCount> core_cntpct_{};
    std::uint32_t cntfrq_el0_ = kCntfrqEl0;
};

}  // namespace zlong::cpu
