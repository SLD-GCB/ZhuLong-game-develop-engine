#include "zlong/ram/address_space.h"

namespace zlong::ram {

namespace {

/// Index shifts for levels 0, 1 and 2. Level 3 indexes at the page shift.
constexpr unsigned kShifts[3] = {39, 30, 21};
constexpr std::uint64_t kIndexMask = 0x1FF;
constexpr std::uint64_t kEntryBytes = sizeof(std::uint64_t);

}  // namespace

AddressSpace::AddressSpace(Ram& ram) : ram_(ram) {}

void AddressSpace::store(std::uint64_t entry_pa, std::uint64_t descriptor) {
    ram_.physical().write64(entry_pa, descriptor);
    // The tables are guest-visible memory and the software TLB caches decoded
    // mappings, so it has to be told.
    ram_.mmu().invalidate_all();
}

bool AddressSpace::enable(std::uint32_t t0sz) {
    const auto root = ram_.physical().allocate(Mmu::kPageSize);
    if (!root.has_value()) {
        return false;
    }
    root0_ = *root;

    ram_.registers().set_ttbr0_el1(root0_);
    ram_.registers().set_ttbr1_el1(root0_);
    const std::uint64_t tcr =
        static_cast<std::uint64_t>(t0sz) | (static_cast<std::uint64_t>(t0sz) << 16);
    ram_.registers().set_tcr_el1(tcr);
    ram_.registers().set_sctlr_el1(1);  // SCTLR_EL1.M
    ram_.mmu().invalidate_all();
    return true;
}

std::uint64_t AddressSpace::allocate_page() {
    const auto pa = ram_.physical().allocate(Mmu::kPageSize);
    return pa.has_value() ? *pa : 0;
}

std::uint64_t AddressSpace::table_for(std::uint64_t va, int level) {
    std::uint64_t table = root0_;
    for (int current = 0; current < level && current < 3; ++current) {
        const std::uint64_t index = (va >> kShifts[current]) & kIndexMask;
        const std::uint64_t entry = table + index * kEntryBytes;

        std::uint64_t raw = 0;
        if (!ram_.physical().read64(entry, raw)) {
            return 0;
        }
        if (ClassifyDescriptor(raw) == DescriptorType::Invalid) {
            const std::uint64_t next = allocate_page();
            if (next == 0) {
                return 0;
            }
            raw = pt::Table(next);
            ram_.physical().write64(entry, raw);
        } else if (ClassifyDescriptor(raw) != DescriptorType::TableOrPage) {
            return 0;  // a block already covers this address
        }
        table = raw & kDescriptorAddressMask;
    }
    return table;
}

bool AddressSpace::map_page(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs) {
    const std::uint64_t l3 = table_for(va, 3);
    if (l3 == 0) {
        return false;
    }
    store(l3 + ((va >> Mmu::kPageShift) & kIndexMask) * kEntryBytes, pt::Page(pa, attrs));
    return true;
}

bool AddressSpace::map_block_2mb(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs) {
    const std::uint64_t l2 = table_for(va, 2);
    if (l2 == 0) {
        return false;
    }
    store(l2 + ((va >> 21) & kIndexMask) * kEntryBytes, pt::Block(pa, attrs));
    return true;
}

bool AddressSpace::map_block_1gb(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs) {
    const std::uint64_t l1 = table_for(va, 1);
    if (l1 == 0) {
        return false;
    }
    store(l1 + ((va >> 30) & kIndexMask) * kEntryBytes, pt::Block(pa, attrs));
    return true;
}

bool AddressSpace::unmap_page(std::uint64_t va) {
    // table_for creates tables that are missing, which is right here: a page
    // that was mapped had its tables made when it was mapped.
    const std::uint64_t l3 = table_for(va, 3);
    if (l3 == 0) {
        return false;
    }
    store(l3 + ((va >> Mmu::kPageShift) & kIndexMask) * kEntryBytes, 0);
    return true;
}

}  // namespace zlong::ram
