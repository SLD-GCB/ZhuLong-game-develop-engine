#include "zlong/ram/ram.h"

#include <algorithm>
#include <utility>

namespace zlong::ram {

namespace {

/// Privilege level of this host thread. A host thread is pinned to one guest
/// core, so thread-local is the right place for per-core state. The kernel sets
/// this when it enters an exception level; EL0 is where application code runs.
thread_local std::uint32_t g_current_el = 0;

/// Detail of the most recent failed access on this thread. Thread-local for the
/// same reason, and because it makes the failing access and the CPU layer's
/// immediate DescribeFault() query race-free across the four cores.
struct LastFault {
    VAddr va = 0;
    bool is_write = false;
    bool is_fetch = false;
    zlong::cpu::FaultInfo info{};
    bool valid = false;
};
thread_local LastFault g_last_fault;

std::uint64_t PageBytesLeft(VAddr va) noexcept {
    return static_cast<std::uint64_t>(Mmu::kPageSize - (va & Mmu::kPageMask));
}

}  // namespace

Ram::Ram(std::vector<PhysicalRegion> regions)
    : physical_(std::move(regions)), mmu_(physical_, registers_) {}

Ram::Ram(std::uint64_t dram_size) : Ram(DefaultLayout(dram_size)) {}

void Ram::set_current_el(std::uint32_t el) noexcept {
    g_current_el = el;
}

std::uint32_t Ram::current_el() const noexcept {
    return g_current_el;
}

void Ram::record_fault(VAddr va, bool is_write, bool is_fetch, const TranslateResult& result) {
    g_last_fault.va = va;
    g_last_fault.is_write = is_write;
    g_last_fault.is_fetch = is_fetch;
    g_last_fault.info.kind = result.kind;
    g_last_fault.info.level = result.level;
    g_last_fault.valid = true;
}

zlong::cpu::FaultInfo Ram::DescribeFault(VAddr address, bool is_write, bool is_fetch) const {
    if (g_last_fault.valid && g_last_fault.va == address && g_last_fault.is_write == is_write &&
        g_last_fault.is_fetch == is_fetch) {
        return g_last_fault.info;
    }
    return {};
}

// ------------------------------------------------------------ data access ---

template <typename T>
std::optional<T> Ram::read(VAddr address) {
    constexpr std::size_t kSize = sizeof(T);
    std::uint64_t accumulated = 0;
    std::size_t done = 0;
    while (done < kSize) {
        const VAddr current = address + done;
        // dynarmic hands us one callback per access, so an access straddling a
        // page boundary has to be split here.
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(kSize - done, PageBytesLeft(current)));
        const TranslateResult translated = mmu_.translate(current, /*is_write=*/false,
                                                          /*is_fetch=*/false, el0());
        if (!translated.ok) {
            record_fault(address, false, false, translated);
            return std::nullopt;
        }
        std::uint64_t part = 0;
        if (!physical_.read(translated.pa, &part, chunk)) {
            TranslateResult unmapped;
            unmapped.kind = zlong::cpu::MemoryFaultKind::Translation;
            record_fault(address, false, false, unmapped);
            return std::nullopt;
        }
        accumulated |= part << (8 * done);
        done += chunk;
    }
    return static_cast<T>(accumulated);
}

template <typename T>
bool Ram::write(VAddr address, T value) {
    constexpr std::size_t kSize = sizeof(T);
    bool touches_code = false;
    bool touches_page_tables = false;
    std::size_t done = 0;
    while (done < kSize) {
        const VAddr current = address + done;
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(kSize - done, PageBytesLeft(current)));
        const TranslateResult translated = mmu_.translate(current, /*is_write=*/true,
                                                          /*is_fetch=*/false, el0());
        if (!translated.ok) {
            record_fault(address, true, false, translated);
            return false;
        }
        const std::uint64_t part = static_cast<std::uint64_t>(value) >> (8 * done);
        if (!physical_.write(translated.pa, &part, chunk)) {
            TranslateResult unmapped;
            unmapped.kind = zlong::cpu::MemoryFaultKind::Translation;
            record_fault(address, true, false, unmapped);
            return false;
        }
        touches_code = touches_code || translated.executable;
        touches_page_tables = touches_page_tables || mmu_.is_walked_page(translated.pa);
        done += chunk;
    }

    // Writing page-table memory can change permissions or executability
    // anywhere, so nothing translated can be trusted. Otherwise, an executable
    // page means the JIT may hold a translation of what we just overwrote
    // (self-modifying code).
    if (touches_page_tables) {
        NotifyCodeInvalidateAll();
    }
    if (touches_code) {
        NotifyCodeWrite(address, kSize);
    }
    return true;
}

std::optional<std::uint8_t> Ram::TryRead8(VAddr address) {
    return read<std::uint8_t>(address);
}

std::optional<std::uint16_t> Ram::TryRead16(VAddr address) {
    return read<std::uint16_t>(address);
}

std::optional<std::uint32_t> Ram::TryRead32(VAddr address) {
    return read<std::uint32_t>(address);
}

std::optional<std::uint64_t> Ram::TryRead64(VAddr address) {
    return read<std::uint64_t>(address);
}

std::optional<zlong::cpu::Vector> Ram::TryRead128(VAddr address) {
    const auto low = read<std::uint64_t>(address);
    if (!low) {
        return std::nullopt;
    }
    const auto high = read<std::uint64_t>(address + 8);
    if (!high) {
        return std::nullopt;
    }
    return zlong::cpu::Vector{*low, *high};
}

bool Ram::TryWrite8(VAddr address, std::uint8_t value) {
    return write<std::uint8_t>(address, value);
}

bool Ram::TryWrite16(VAddr address, std::uint16_t value) {
    return write<std::uint16_t>(address, value);
}

bool Ram::TryWrite32(VAddr address, std::uint32_t value) {
    return write<std::uint32_t>(address, value);
}

bool Ram::TryWrite64(VAddr address, std::uint64_t value) {
    return write<std::uint64_t>(address, value);
}

bool Ram::TryWrite128(VAddr address, zlong::cpu::Vector value) {
    return write<std::uint64_t>(address, value[0]) && write<std::uint64_t>(address + 8, value[1]);
}

std::optional<std::uint32_t> Ram::TryFetch32(VAddr address) {
    const TranslateResult translated = mmu_.translate(address, /*is_write=*/false,
                                                      /*is_fetch=*/true, el0());
    if (!translated.ok) {
        record_fault(address, false, true, translated);
        return std::nullopt;
    }
    // 4-byte aligned, so it cannot straddle a page.
    std::uint32_t word = 0;
    if (!physical_.read(translated.pa, &word, sizeof(word))) {
        TranslateResult unmapped;
        unmapped.kind = zlong::cpu::MemoryFaultKind::Translation;
        record_fault(address, false, true, unmapped);
        return std::nullopt;
    }
    return word;
}

// -------------------------------------------------- system instructions ----

std::optional<std::uint64_t> Ram::read_system_register(std::size_t core, std::uint32_t key) const {
    switch (key) {
    case SystemRegKey(0, 0, 0, 0):  // MIDR_EL1
        return SystemRegisters::kMidrEl1;
    case SystemRegKey(0, 0, 0, 5):  // MPIDR_EL1 (Aff0 = core)
        return 0x8000'0000ULL | static_cast<std::uint64_t>(core);
    case SystemRegKey(0, 4, 2, 2):  // CurrentEL
        return static_cast<std::uint64_t>(current_el()) << 2;
    case SystemRegKey(0, 1, 0, 0):  // SCTLR_EL1
        return registers_.sctlr_el1();
    case SystemRegKey(0, 1, 0, 2):  // CPACR_EL1
        return registers_.cpacr_el1();
    case SystemRegKey(0, 2, 0, 0):  // TTBR0_EL1
        return registers_.ttbr0_el1();
    case SystemRegKey(0, 2, 0, 1):  // TTBR1_EL1
        return registers_.ttbr1_el1();
    case SystemRegKey(0, 2, 0, 2):  // TCR_EL1
        return registers_.tcr_el1();
    case SystemRegKey(0, 10, 2, 0):  // MAIR_EL1
        return registers_.mair_el1();
    default:
        // Exception-state registers (ESR/FAR/VBAR/ELR/SPSR_EL1) belong to the
        // kernel's exception model, not to memory management.
        return std::nullopt;
    }
}

bool Ram::write_system_register(std::uint32_t key, std::uint64_t value) {
    switch (key) {
    case SystemRegKey(0, 1, 0, 0):
        registers_.set_sctlr_el1(value);
        break;
    case SystemRegKey(0, 1, 0, 2):
        registers_.set_cpacr_el1(value);
        break;
    case SystemRegKey(0, 2, 0, 0):
        registers_.set_ttbr0_el1(value);
        break;
    case SystemRegKey(0, 2, 0, 1):
        registers_.set_ttbr1_el1(value);
        break;
    case SystemRegKey(0, 2, 0, 2):
        registers_.set_tcr_el1(value);
        break;
    case SystemRegKey(0, 10, 2, 0):
        registers_.set_mair_el1(value);
        break;
    default:
        return false;
    }
    mmu_.invalidate_all();
    return true;
}

zlong::cpu::SystemInsnResult Ram::HandleSystemInstruction(std::size_t core, VAddr /*pc*/,
                                                          std::uint32_t instruction,
                                                          std::uint64_t rt_value) {
    using zlong::cpu::SystemInsnResult;

    constexpr std::uint32_t kMrsMask = 0xFFF0'0000u;
    constexpr std::uint32_t kMrsMatch = 0xD530'0000u;
    constexpr std::uint32_t kMsrMask = 0xFFF0'0000u;
    constexpr std::uint32_t kMsrMatch = 0xD510'0000u;
    // The SYS opcode includes bit 19, so the mask must cover bits 31..19 --
    // 0xFFE00000 would wrongly match (verified: tlbi vmalle1 == 0xD508871F).
    constexpr std::uint32_t kSysMask = 0xFFF8'0000u;
    constexpr std::uint32_t kSysMatch = 0xD508'0000u;

    SystemInsnResult result;
    const std::uint32_t key = SystemRegKey((instruction >> 16) & 0x7u, (instruction >> 12) & 0xFu,
                                           (instruction >> 8) & 0xFu, (instruction >> 5) & 0x7u);

    if ((instruction & kMrsMask) == kMrsMatch) {
        if (const auto value = read_system_register(core, key)) {
            result.kind = SystemInsnResult::Kind::Handled;
            result.write_rt = static_cast<std::uint8_t>(instruction & 0x1Fu);
            result.write_value = *value;
        }
        return result;
    }

    if ((instruction & kMsrMask) == kMsrMatch) {
        if (write_system_register(key, rt_value)) {
            result.kind = SystemInsnResult::Kind::Handled;
        }
        return result;
    }

    if ((instruction & kSysMask) == kSysMatch) {
        // CRn == 8 is the TLBI group. Every form just drops all translations
        // (conservative, and the forms are numerous).
        if (((instruction >> 12) & 0xFu) == 8u) {
            mmu_.invalidate_all();
            result.kind = SystemInsnResult::Kind::Handled;
        }
        return result;
    }

    return result;
}

}  // namespace zlong::ram
