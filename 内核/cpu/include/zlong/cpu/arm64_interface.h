// 烛龙 (ZhuLong) - dynarmic UserCallbacks adapter for one guest core.
//
// This class holds no state of its own: it forwards to the Core. Every memory
// callback obeys the no-throw contract documented in memory.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <dynarmic/interface/A64/config.h>

namespace zlong::cpu {

class Core;

class Arm64Interface final : public Dynarmic::A64::UserCallbacks {
public:
    explicit Arm64Interface(Core& core) noexcept : core_(core) {}

    // Instruction fetch. Overridden so that fetch permissions are distinct from
    // data permissions; std::nullopt becomes Exception::NoExecuteFault.
    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A64::VAddr vaddr) override;

    std::uint8_t MemoryRead8(Dynarmic::A64::VAddr vaddr) override;
    std::uint16_t MemoryRead16(Dynarmic::A64::VAddr vaddr) override;
    std::uint32_t MemoryRead32(Dynarmic::A64::VAddr vaddr) override;
    std::uint64_t MemoryRead64(Dynarmic::A64::VAddr vaddr) override;
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
    std::uint64_t GetTicksRemaining() override;
    std::uint64_t GetCNTPCT() override;

private:
    std::uint64_t ReadBytes(Dynarmic::A64::VAddr vaddr, std::size_t n);
    void WriteBytes(Dynarmic::A64::VAddr vaddr, std::size_t n, std::uint64_t value);
    void ReportFault(Dynarmic::A64::VAddr vaddr, std::size_t n, bool is_write, bool is_fetch);

    Core& core_;
};

}  // namespace zlong::cpu
