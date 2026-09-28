#include "zlong/ram/mmu.h"

#include <algorithm>
#include <array>

namespace zlong::ram {

namespace {

// Descriptor bits and the type rule come from mmu.h: one definition of the
// format, shared with the mapping side.
constexpr unsigned kIndexBits = 9;      // 4 KB granule
constexpr std::uint64_t kIndexMask = 0x1FF;

constexpr std::size_t kTlbEntries = 4096;

/// Direct-mapped software TLB, one per host thread. A host thread is pinned to a
/// guest core, so this is per-core without any locking. Entries are invalidated
/// by comparing a global generation counter rather than by broadcasting.
struct TlbEntry {
    std::uint64_t owner_id;   // 0 == empty; Mmu ids start at 1
    std::uint64_t generation;
    std::uint64_t va_page;
    std::uint64_t pa_page;
    std::uint8_t level;
    std::uint8_t ap;
    bool af;
    bool uxn;
    bool pxn;
};

thread_local std::array<TlbEntry, kTlbEntries> g_tlb{};  // zero-init: owner_id == 0

/// Hands out the unique ids used to tell Mmu instances apart in the TLB.
std::atomic<std::uint64_t> g_next_mmu_id{1};

/// VA size covered by TTBR0 with this T0SZ. T0SZ == 0 covers the whole space.
bool covered_by_ttbr0(std::uint64_t va, std::uint32_t t0sz) noexcept {
    if (t0sz == 0) {
        return true;
    }
    const std::uint32_t bits = 64 - t0sz;
    if (bits >= 64) {
        return true;
    }
    return va < (1ULL << bits);
}

/// VA size covered by TTBR1 with this T1SZ (the top of the address space).
bool covered_by_ttbr1(std::uint64_t va, std::uint32_t t1sz) noexcept {
    if (t1sz == 0) {
        return false;  // empty range
    }
    const std::uint32_t bits = 64 - t1sz;
    if (bits >= 64) {
        return false;
    }
    const std::uint64_t base = ~((1ULL << bits) - 1);
    return va >= base;
}

}  // namespace

Mmu::Mmu(PhysicalMemory& physical, SystemRegisters& registers)
    : physical_(physical), registers_(registers) {
    id_ = g_next_mmu_id.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t max_end = 0;
    for (const auto& region : physical_.regions()) {
        max_end = std::max(max_end, region.base + region.size);
    }
    const std::size_t pages = static_cast<std::size_t>((max_end >> kPageShift) + 1);
    walked_words_ = (pages + 63) / 64;
    walked_ = std::make_unique<std::atomic<std::uint64_t>[]>(walked_words_);
    for (std::size_t i = 0; i < walked_words_; ++i) {
        walked_[i].store(0, std::memory_order_relaxed);
    }
}

void Mmu::set_translation(std::uint64_t ttbr0, std::uint64_t tcr, std::uint64_t sctlr) noexcept {
    registers_.set_ttbr0_el1(ttbr0);
    registers_.set_tcr_el1(tcr);
    registers_.set_sctlr_el1(sctlr);
    invalidate_all();
}

void Mmu::mark_walked_page(std::uint64_t pa, std::size_t n) noexcept {
    if (walked_ == nullptr || n == 0) {
        return;
    }
    const std::uint64_t first = pa >> kPageShift;
    const std::uint64_t last = (pa + n - 1) >> kPageShift;
    for (std::uint64_t page = first; page <= last; ++page) {
        const std::size_t word = static_cast<std::size_t>(page / 64);
        if (word >= walked_words_) {
            return;
        }
        walked_[word].fetch_or(1ULL << (page % 64), std::memory_order_relaxed);
    }
}

bool Mmu::is_walked_page(std::uint64_t pa) const noexcept {
    if (walked_ == nullptr) {
        return false;
    }
    const std::uint64_t page = pa >> kPageShift;
    const std::size_t word = static_cast<std::size_t>(page / 64);
    if (word >= walked_words_) {
        return false;
    }
    return (walked_[word].load(std::memory_order_relaxed) & (1ULL << (page % 64))) != 0;
}

bool Mmu::check_permissions(const Descriptor& desc, bool is_write, bool is_fetch, bool el0,
                            TranslateResult& result) const {
    if (!desc.af) {
        result.ok = false;
        result.kind = MemoryFaultKind::Permission;
        return false;
    }
    const bool user_accessible = (desc.ap & 0b10) != 0;  // AP[1]
    const bool read_only = (desc.ap & 0b01) != 0;        // AP[2]

    if (el0 && !user_accessible) {
        result.ok = false;
        result.kind = MemoryFaultKind::Permission;
        return false;
    }
    if (is_write && read_only) {
        result.ok = false;
        result.kind = MemoryFaultKind::Permission;
        return false;
    }
    if (is_fetch) {
        if (el0 && desc.uxn) {
            result.ok = false;
            result.kind = MemoryFaultKind::Permission;
            return false;
        }
        if (!el0 && desc.pxn) {
            result.ok = false;
            result.kind = MemoryFaultKind::Permission;
            return false;
        }
    }
    return true;
}

TranslateResult Mmu::walk(VAddr va, bool is_write, bool is_fetch, bool el0, Descriptor& out) {
    TranslateResult result;

    const std::uint32_t t0sz = registers_.t0sz();
    const std::uint32_t t1sz = registers_.t1sz();

    std::uint64_t ttbr = 0;
    std::uint32_t tnsz = 0;
    if (covered_by_ttbr0(va, t0sz)) {
        ttbr = registers_.ttbr0_el1();
        tnsz = t0sz;
    } else if (covered_by_ttbr1(va, t1sz)) {
        ttbr = registers_.ttbr1_el1();
        tnsz = t1sz;
    } else {
        result.kind = MemoryFaultKind::AddressSize;
        result.level = 0;
        return result;
    }

    const std::uint32_t va_bits = 64 - tnsz;
    const std::uint64_t va_mask = (va_bits >= 64) ? ~0ULL : ((1ULL << va_bits) - 1);
    const VAddr addr = va & va_mask;

    const int levels = static_cast<int>((va_bits - kPageShift + kIndexBits - 1) / kIndexBits);
    if (levels > 4 || levels < 1) {
        result.kind = MemoryFaultKind::AddressSize;
        result.level = 0;
        return result;
    }
    int level = 4 - levels;

    std::uint64_t table = ttbr & kDescriptorAddressMask;
    for (; level <= 3; ++level) {
        const unsigned shift = kIndexBits * (3 - level) + kPageShift;
        const std::uint64_t index = (addr >> shift) & kIndexMask;
        const std::uint64_t entry_pa = table + index * sizeof(std::uint64_t);
        mark_walked_page(entry_pa, sizeof(std::uint64_t));

        std::uint64_t raw = 0;
        if (!physical_.read64(entry_pa, raw)) {
            result.kind = MemoryFaultKind::Translation;
            result.level = static_cast<std::uint8_t>(level);
            return result;
        }
        if ((raw & kDescriptorValidBit) == 0) {
            result.kind = MemoryFaultKind::Translation;
            result.level = static_cast<std::uint8_t>(level);
            return result;
        }

        Descriptor desc;
        desc.raw = raw;
        desc.af = (raw & kDescriptorAfBit) != 0;
        desc.ap = static_cast<std::uint8_t>((raw >> 6) & 0b11);
        desc.uxn = (raw & kDescriptorUxnBit) != 0;
        desc.pxn = (raw & kDescriptorPxnBit) != 0;
        out = desc;
        result.level = static_cast<std::uint8_t>(level);

        const std::uint64_t table_or_block_bits = kIndexBits * (3 - level) + kPageShift;

        // ARMv8-A type bits, via the one definition in mmu.h. Reading them the
        // other way round is silent: L3 pages are unaffected, and a table and a
        // block simply swap meanings.
        switch (ClassifyDescriptor(raw)) {
        case DescriptorType::Invalid:
            result.kind = MemoryFaultKind::Translation;
            result.level = static_cast<std::uint8_t>(level);
            return result;

        case DescriptorType::TableOrPage:
            if (level == 3) {
                break;  // a page: a leaf, so fall through to the mapping
            }
            table = raw & kDescriptorAddressMask;
            continue;  // a table: descend

        case DescriptorType::Block:
            // Blocks exist only at levels 0..2.
            if (level == 3) {
                result.kind = MemoryFaultKind::Translation;
                result.level = 3;
                return result;
            }
            break;  // a leaf
        }

        if (!check_permissions(desc, is_write, is_fetch, el0, result)) {
            return result;
        }
        const std::uint64_t extent = 1ULL << table_or_block_bits;
        result.ok = true;
        result.pa = ((raw & kDescriptorAddressMask) & ~(extent - 1)) | (va & (extent - 1));
        result.executable = !(desc.uxn && desc.pxn);
        result.writable = !((desc.ap & 0b01) != 0);
        return result;
    }

    result.kind = MemoryFaultKind::Translation;
    result.level = 3;
    return result;
}

TranslateResult Mmu::translate(VAddr va, bool is_write, bool is_fetch, bool el0) {
    // Translation off: VA == PA, straight into physical memory.
    if (!registers_.mmu_enabled()) {
        TranslateResult result;
        if (!physical_.mapped(va, 1)) {
            result.kind = MemoryFaultKind::Translation;
            return result;
        }
        result.ok = true;
        result.pa = va;
        result.level = 0;
        result.executable = physical_.is_executable(va);
        result.writable = true;
        return result;
    }

    const std::uint64_t generation = this->generation();
    const std::uint64_t page = va >> kPageShift;
    TlbEntry& slot = g_tlb[static_cast<std::size_t>(page % kTlbEntries)];

    // A hit still re-runs the permission check: the cached entry holds the
    // mapping's attributes, not a verdict for one particular access.
    if (slot.owner_id == id_ && slot.generation == generation && slot.va_page == page) {
        Descriptor desc;
        desc.af = slot.af;
        desc.ap = slot.ap;
        desc.uxn = slot.uxn;
        desc.pxn = slot.pxn;

        TranslateResult result;
        result.level = slot.level;
        if (!check_permissions(desc, is_write, is_fetch, el0, result)) {
            return result;
        }
        result.ok = true;
        result.pa = slot.pa_page | (va & kPageMask);
        result.executable = !(desc.uxn && desc.pxn);
        result.writable = !((desc.ap & 0b01) != 0);
        return result;
    }

    Descriptor desc;
    TranslateResult result = walk(va, is_write, is_fetch, el0, desc);
    if (result.ok) {
        slot.owner_id = id_;
        slot.generation = generation;
        slot.va_page = page;
        slot.pa_page = result.pa & ~kPageMask;
        slot.level = result.level;
        slot.ap = desc.ap;
        slot.af = desc.af;
        slot.uxn = desc.uxn;
        slot.pxn = desc.pxn;
    }
    return result;
}

}  // namespace zlong::ram
