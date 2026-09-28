// 烛龙 (ZhuLong) - the RAM layer: guest physical memory plus a real MMU,
// exposed to the CPU layer as a GuestMemory bus.
//
// Addresses arriving here are guest VIRTUAL addresses (dynarmic does no
// translation). Everything the four cores touch goes through this object.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zlong/cpu/memory.h"
#include "zlong/cpu/types.h"
#include "zlong/ram/mmu.h"
#include "zlong/ram/physical.h"
#include "zlong/ram/registers.h"

namespace zlong::ram {

class Ram final : public zlong::cpu::GuestMemory {
public:
    // No default argument on purpose: the default layout is 4 GiB and backing it
    // allocates eagerly, so callers state what they want.
    explicit Ram(std::vector<PhysicalRegion> regions);
    explicit Ram(std::uint64_t dram_size);

    PhysicalMemory& physical() noexcept { return physical_; }
    Mmu& mmu() noexcept { return mmu_; }
    SystemRegisters& registers() noexcept { return registers_; }

    /// Privilege used for permission checks on this host thread. The kernel
    /// calls this when it enters an exception level; default is EL0, which is
    /// where guest application code runs.
    void set_current_el(std::uint32_t el) noexcept;
    std::uint32_t current_el() const noexcept;

    // --- GuestMemory (virtual addresses; never throws) ---------------------
    std::optional<std::uint8_t> TryRead8(VAddr address) override;
    std::optional<std::uint16_t> TryRead16(VAddr address) override;
    std::optional<std::uint32_t> TryRead32(VAddr address) override;
    std::optional<std::uint64_t> TryRead64(VAddr address) override;
    std::optional<zlong::cpu::Vector> TryRead128(VAddr address) override;

    bool TryWrite8(VAddr address, std::uint8_t value) override;
    bool TryWrite16(VAddr address, std::uint16_t value) override;
    bool TryWrite32(VAddr address, std::uint32_t value) override;
    bool TryWrite64(VAddr address, std::uint64_t value) override;
    bool TryWrite128(VAddr address, zlong::cpu::Vector value) override;

    std::optional<std::uint32_t> TryFetch32(VAddr address) override;

    zlong::cpu::FaultInfo DescribeFault(VAddr address, bool is_write, bool is_fetch) const override;
    zlong::cpu::SystemInsnResult HandleSystemInstruction(std::size_t core, VAddr pc,
                                                         std::uint32_t instruction,
                                                         std::uint64_t rt_value) override;

private:
    template <typename T>
    std::optional<T> read(VAddr address);
    template <typename T>
    bool write(VAddr address, T value);

    std::optional<std::uint64_t> read_system_register(std::size_t core, std::uint32_t key) const;
    bool write_system_register(std::uint32_t key, std::uint64_t value);

    void record_fault(VAddr va, bool is_write, bool is_fetch, const TranslateResult& result);

    bool el0() const noexcept { return current_el() == 0; }

    PhysicalMemory physical_;
    SystemRegisters registers_;
    Mmu mmu_;
};

}  // namespace zlong::ram
