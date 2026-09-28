// 烛龙 (ZhuLong) - ARMv8-A stage-1 address translation.
//
// Scope: 4 KB granule, 4-level walk, TTBR0/TTBR1 selected by TCR_EL1.T0SZ/T1SZ,
// AP/AF/UXN/PXN permission checks, stage-1 only, no hardware access-flag
// update, no contiguous hints.
//
// Deliberately NOT using dynarmic's page_table/fastmem fast path: a non-null
// page-table entry or fastmem-covered access becomes a bare host load/store
// with no callback at all, and dynarmic exposes no write-watch or page-table-
// miss hook. For guest RAM that would silently break self-modifying-code
// detection (see 内核/cpu CodeWriteObserver). The software TLB below is the
// performance mechanism instead.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "zlong/cpu/types.h"
#include "zlong/ram/physical.h"
#include "zlong/ram/registers.h"

namespace zlong::ram {

using zlong::cpu::MemoryFaultKind;
using zlong::cpu::VAddr;

// ---------------------------------------------------------------------------
// The descriptor format, written down once.
//
// `mmu.cpp` walks on these and the mapping side builds on them, so there is one
// definition of the format rather than two that can drift apart.
//
// ARMv8-A type bits:
//   bits[1:0] == 0b11  ->  a Table at levels 0..2, and a Page at level 3
//   bits[1:0] == 0b01  ->  a Block, which exists only at levels 0..2
//   otherwise          ->  not a valid descriptor
//
// Reading "bit 1 set means leaf" is the tempting mistake, and it is silent: L3
// pages still work (0b11 is a page there), while tables and blocks simply swap
// meanings -- so a real guest page table walks into garbage.
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kDescriptorValidBit = 1ULL << 0;
inline constexpr std::uint64_t kDescriptorTypeBit = 1ULL << 1;
inline constexpr std::uint64_t kDescriptorAfBit = 1ULL << 10;
inline constexpr std::uint64_t kDescriptorPxnBit = 1ULL << 53;
inline constexpr std::uint64_t kDescriptorUxnBit = 1ULL << 54;
inline constexpr std::uint64_t kDescriptorAddressMask = 0x0000'FFFF'FFFF'F000ULL;

enum class DescriptorType : std::uint8_t {
    Invalid,
    /// bits[1:0] == 0b01. Levels 0..2 only.
    Block,
    /// bits[1:0] == 0b11. A table at levels 0..2, a page at level 3.
    TableOrPage,
};

constexpr DescriptorType ClassifyDescriptor(std::uint64_t raw) noexcept {
    if ((raw & kDescriptorValidBit) == 0) {
        return DescriptorType::Invalid;
    }
    return (raw & kDescriptorTypeBit) != 0 ? DescriptorType::TableOrPage
                                           : DescriptorType::Block;
}

struct TranslateResult {
    bool ok = false;
    std::uint64_t pa = 0;
    MemoryFaultKind kind = MemoryFaultKind::Translation;
    /// Translation level that faulted, or the level that produced the mapping.
    std::uint8_t level = 0;
    /// Whether the mapping is executable at *some* privilege. Conservative
    /// (set when not both UXN and PXN are set) because it gates code-cache
    /// invalidation on writes.
    bool executable = false;
    bool writable = false;
};

class Mmu {
public:
    static constexpr std::size_t kPageShift = 12;
    static constexpr std::uint64_t kPageSize = 1ULL << kPageShift;
    static constexpr std::uint64_t kPageMask = kPageSize - 1;

    Mmu(PhysicalMemory& physical, SystemRegisters& registers);

    /// Translate a guest virtual address. `el0` selects the privilege level used
    /// for permission checks.
    TranslateResult translate(VAddr va, bool is_write, bool is_fetch, bool el0);

    /// Drop every cached translation. Called when the guest writes a register
    /// that affects translation, executes TLBI, or modifies page-table memory.
    void invalidate_all() noexcept { generation_.fetch_add(1, std::memory_order_acq_rel); }

    std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    /// True when this 4 KB page has been visited as a translation table during a
    /// walk. A write to such a page may have changed permissions anywhere, so the
    /// caller flushes everything.
    bool is_walked_page(std::uint64_t pa) const noexcept;

    /// Convenience for tests/kernel: program a translation regime.
    void set_translation(std::uint64_t ttbr0, std::uint64_t tcr, std::uint64_t sctlr) noexcept;

private:
    struct Descriptor {
        std::uint64_t raw = 0;
        bool af = false;
        bool uxn = false;
        bool pxn = false;
        std::uint8_t ap = 0;
    };

    TranslateResult walk(VAddr va, bool is_write, bool is_fetch, bool el0, Descriptor& out);
    bool check_permissions(const Descriptor& desc, bool is_write, bool is_fetch, bool el0,
                           TranslateResult& result) const;

    void mark_walked_page(std::uint64_t pa, std::size_t n) noexcept;

    PhysicalMemory& physical_;
    SystemRegisters& registers_;
    /// Unique identity for TLB ownership. Deliberately NOT the object address:
    /// a new Mmu can be constructed at the address of a destroyed one, and a
    /// stale TLB entry from the old instance would then be treated as a hit.
    std::uint64_t id_ = 0;
    std::atomic<std::uint64_t> generation_{1};
    // One bit per 4 KB page, indexed by absolute page number. unique_ptr array
    // rather than a vector because std::atomic is neither copyable nor movable.
    std::unique_ptr<std::atomic<std::uint64_t>[]> walked_;
    std::size_t walked_words_ = 0;
};

}  // namespace zlong::ram
