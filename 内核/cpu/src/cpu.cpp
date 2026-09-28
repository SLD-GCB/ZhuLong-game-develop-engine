#include "zlong/cpu/cpu.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <thread>

namespace zlong::cpu {

Cpu::Cpu(GuestMemory& memory, CpuHost& host) : memory_(memory), host_(host) {
    for (std::size_t i = 0; i < kCoreCount; ++i) {
        core_cntpct_[i].store(0, std::memory_order_relaxed);
        cores_[i] = std::make_unique<Core>(*this, i, memory_);
    }
    // Registered last, so the cores exist before anything can report code
    // writes to them.
    memory_.SetCodeWriteObserver(this);
}

Cpu::~Cpu() {
    StopAll();
    memory_.SetCodeWriteObserver(nullptr);
    // Cores must die before exclusive_monitor_ (member order guarantees it, but
    // be explicit about intent).
    cores_ = {};
}

void Cpu::StartAll() {
    for (auto& core : cores_) {
        if (core) {
            core->Start();
        }
    }
}

void Cpu::StopAll() {
    for (auto& core : cores_) {
        if (core) {
            core->RequestStop();
        }
    }
}

Core& Cpu::GetCore(std::size_t index) {
    if (index >= kCoreCount) {
        throw std::out_of_range("core index out of range");
    }
    return *cores_[index];
}

const Core& Cpu::GetCore(std::size_t index) const {
    if (index >= kCoreCount) {
        throw std::out_of_range("core index out of range");
    }
    return *cores_[index];
}

void Cpu::OnGuestCodeWrite(VAddr address, std::size_t length) {
    // Every core has its own code cache, so a write that one core made to
    // executable memory invalidates the translations held by all four.
    for (auto& core : cores_) {
        if (core) {
            core->GetJit().InvalidateCacheRange(address, length);
        }
    }
}

void Cpu::OnGuestCodeInvalidateAll() {
    // ClearCache rather than InvalidateCacheRange over the whole space: a
    // length of SIZE_MAX overflows dynarmic's end_address computation.
    for (auto& core : cores_) {
        if (core) {
            core->GetJit().ClearCache();
        }
    }
}

void Cpu::BroadcastEvent() {
    for (auto& core : cores_) {
        if (core) {
            core->SetEventRegister();
        }
    }
}

std::uint64_t Cpu::GetCNTPCT() const noexcept {
    return cntpct_.load(std::memory_order_acquire);
}

void Cpu::AdvanceCNTPCT(std::uint64_t ticks) noexcept {
    cntpct_.fetch_add(ticks, std::memory_order_relaxed);
}

std::uint64_t Cpu::TicksPerFrame() const noexcept {
    return static_cast<std::uint64_t>(cntfrq_el0_) / 60u;
}

void Cpu::ReportFrameBoundary(std::size_t index) {
    const std::uint64_t now = GetCNTPCT();
    if (index < kCoreCount) {
        core_cntpct_[index].store(now, std::memory_order_relaxed);
    }

    // Advisory skew hint only. A hard rendezvous would deadlock the moment one
    // core parks in WFE or blocks inside a syscall, so this can only ever yield.
    std::uint64_t slowest = now;
    for (const auto& published : core_cntpct_) {
        const std::uint64_t value = published.load(std::memory_order_relaxed);
        if (value != 0) {
            slowest = std::min(slowest, value);
        }
    }
    if (now > slowest + TicksPerFrame() * 2) {
        std::this_thread::yield();
    }
}

}  // namespace zlong::cpu
