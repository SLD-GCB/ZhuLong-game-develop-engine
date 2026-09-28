#include "zlong/service/address_space.h"

#include <algorithm>

namespace zlong::service {

namespace {

constexpr std::uint64_t kPageSize = ram::Mmu::kPageSize;
constexpr std::uint64_t kPageMask = ram::Mmu::kPageSize - 1;

/// Descriptor attributes for a permission set. The format lives in the RAM
/// layer; this only decides which of its parts apply.
std::uint64_t AttributesFor(std::uint32_t permissions) {
    std::uint64_t attrs = ram::pt::kAf;
    // EL0 accessible either way; read-only when write was not asked for.
    attrs |= ram::pt::Ap(/*el0_accessible=*/true, /*read_only=*/(permissions & kMemoryWrite) == 0);
    if ((permissions & kMemoryExecute) == 0) {
        attrs |= ram::pt::kUxn;
    }
    return attrs;
}

bool Aligned(std::uint64_t value) noexcept { return (value & kPageMask) == 0; }

void SetError(std::string& error, const char* text) { error = text; }

}  // namespace

KAddressSpace::KAddressSpace(ram::Ram& ram, ram::PhysicalMemory& physical)
    : ram_(ram), physical_(physical), space_(ram) {}

bool KAddressSpace::Enable() {
    if (enabled_) {
        return true;
    }
    if (!space_.enable()) {
        return false;
    }
    enabled_ = true;
    return true;
}

bool KAddressSpace::Reserve(std::uint64_t base, std::uint64_t size, std::string& error) {
    if (size == 0) {
        SetError(error, "a zero-length range is not a reservation");
        return false;
    }
    if (!Aligned(base) || !Aligned(size)) {
        SetError(error, "the range is not page aligned");
        return false;
    }
    if (base < kFirstGuestAddress || base > kLastGuestAddress ||
        size > kLastGuestAddress - base + 1) {
        SetError(error, "the range is outside the translatable addresses");
        return false;
    }
    for (const Region& region : regions_) {
        // Half-open intervals: touching ranges do not overlap.
        if (base < region.base + region.size && region.base < base + size) {
            SetError(error, "the range overlaps one that already exists");
            return false;
        }
    }

    Region region;
    region.base = base;
    region.size = size;
    regions_.push_back(region);
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& left, const Region& right) { return left.base < right.base; });
    next_free_ = std::max(next_free_, base + size);
    error.clear();
    return true;
}

KAddressSpace::Region* KAddressSpace::FindContaining(std::uint64_t base, std::uint64_t size) {
    for (Region& region : regions_) {
        if (base >= region.base && base + size <= region.base + region.size) {
            return &region;
        }
    }
    return nullptr;
}

KAddressSpace::Region* KAddressSpace::PrepareMapping(std::uint64_t base, std::uint64_t size,
                                                      std::string& error) {
    if (!enabled_) {
        SetError(error, "the address space is not enabled");
        return nullptr;
    }
    if (size == 0 || !Aligned(base) || !Aligned(size)) {
        SetError(error, "the range is empty or not page aligned");
        return nullptr;
    }
    Region* region = FindContaining(base, size);
    if (region == nullptr) {
        // Mapping something the guest never reserved would be a mapping the
        // region table does not know about, and unmap could then never find it.
        SetError(error, "the range was not reserved");
        return nullptr;
    }
    if (region->mapped) {
        SetError(error, "the range is already mapped");
        return nullptr;
    }
    return region;
}

bool KAddressSpace::Map(std::uint64_t base, std::uint64_t size, std::uint32_t permissions,
                        std::string& error) {
    Region* region = PrepareMapping(base, size, error);
    if (region == nullptr) {
        return false;
    }

    const std::uint64_t attributes = AttributesFor(permissions);
    for (std::uint64_t address = base; address < base + size; address += kPageSize) {
        const auto page = physical_.allocate(kPageSize);
        if (!page.has_value()) {
            // Roll back what this call already mapped, so a partial mapping is
            // not left behind for a caller that was told it failed.
            for (std::uint64_t undo = base; undo < address; undo += kPageSize) {
                space_.unmap_page(undo);
            }
            SetError(error, "out of physical memory");
            return false;
        }
        if (!space_.map_page(address, *page, attributes)) {
            for (std::uint64_t undo = base; undo < address; undo += kPageSize) {
                space_.unmap_page(undo);
            }
            SetError(error, "the page tables could not be built");
            return false;
        }
    }

    region->mapped = true;
    region->permissions = permissions;
    error.clear();
    return true;
}

bool KAddressSpace::MapTo(std::uint64_t base, std::uint64_t physical_base, std::uint64_t size,
                          std::uint32_t permissions, std::string& error) {
    Region* region = PrepareMapping(base, size, error);
    if (region == nullptr) {
        return false;
    }
    if (!Aligned(physical_base)) {
        SetError(error, "the physical range is not page aligned");
        return false;
    }

    const std::uint64_t attributes = AttributesFor(permissions);
    for (std::uint64_t offset = 0; offset < size; offset += kPageSize) {
        if (!space_.map_page(base + offset, physical_base + offset, attributes)) {
            for (std::uint64_t undo = 0; undo < offset; undo += kPageSize) {
                space_.unmap_page(base + undo);
            }
            SetError(error, "the page tables could not be built");
            return false;
        }
    }

    region->mapped = true;
    region->permissions = permissions;
    error.clear();
    return true;
}

bool KAddressSpace::LoadImage(std::uint64_t base, const void* bytes, std::size_t size,
                              std::uint32_t permissions, std::string& error) {
    if (size == 0) {
        SetError(error, "the image is empty");
        return false;
    }
    if (bytes == nullptr) {
        SetError(error, "the image has a size but no bytes");
        return false;
    }

    const std::uint64_t rounded = (size + kPageMask) & ~kPageMask;
    if (!Reserve(base, rounded, error)) {
        return false;
    }
    if (!Map(base, rounded, permissions, error)) {
        return false;
    }

    // Page by page, because a store has to land on the physical page the mapping
    // produced rather than at some address the caller guessed. EL1 on purpose:
    // the loader is filling a page the guest may only be allowed to read or
    // execute, and the permission check for the *guest* is not the loader's.
    const auto* source = static_cast<const std::uint8_t*>(bytes);
    for (std::uint64_t offset = 0; offset < size; offset += kPageSize) {
        const ram::TranslateResult translated =
            ram_.mmu().translate(base + offset, /*is_write=*/false, /*is_fetch=*/false,
                                 /*el0=*/false);
        if (!translated.ok) {
            SetError(error, "a page of the image is not mapped");
            return false;
        }
        const std::uint64_t remaining = size - offset;
        const auto chunk = static_cast<std::size_t>(remaining < kPageSize ? remaining : kPageSize);
        if (!physical_.write(translated.pa, source + offset, chunk)) {
            SetError(error, "the image could not be written through the mapping");
            return false;
        }
    }

    error.clear();
    return true;
}

bool KAddressSpace::Unmap(std::uint64_t base, std::uint64_t size, std::string& error) {
    if (size == 0 || !Aligned(base) || !Aligned(size)) {
        SetError(error, "the range is empty or not page aligned");
        return false;
    }
    Region* region = FindContaining(base, size);
    if (region == nullptr || !region->mapped) {
        SetError(error, "the range is not mapped");
        return false;
    }
    // A partial unmap would leave the region table describing something that is
    // no longer true, so only the whole region can be dropped.
    if (base != region->base || size != region->size) {
        SetError(error, "only a whole region can be unmapped");
        return false;
    }

    for (std::uint64_t address = base; address < base + size; address += kPageSize) {
        space_.unmap_page(address);
    }
    // The virtual range goes back to the pool, so it can be reserved again. The
    // physical pages do not: the allocator cannot take them back, which is the
    // documented leak.
    regions_.erase(std::find_if(regions_.begin(), regions_.end(),
                                [base](const Region& candidate) { return candidate.base == base; }));
    error.clear();
    return true;
}

bool KAddressSpace::SetAttributes(std::uint64_t base, std::uint64_t size,
                                  std::uint32_t permissions, std::string& error) {
    if (size == 0 || !Aligned(base) || !Aligned(size)) {
        SetError(error, "the range is empty or not page aligned");
        return false;
    }
    Region* region = FindContaining(base, size);
    if (region == nullptr || !region->mapped) {
        SetError(error, "the range is not mapped");
        return false;
    }
    if (base != region->base || size != region->size) {
        SetError(error, "only a whole region can have its attributes changed");
        return false;
    }

    const std::uint64_t attributes = AttributesFor(permissions);
    for (std::uint64_t address = base; address < base + size; address += kPageSize) {
        const auto translated = ram_.mmu().translate(address, false, false, true);
        if (!translated.ok) {
            SetError(error, "a page in the range is not mapped");
            return false;
        }
        const std::uint64_t page_base = translated.pa & ~kPageMask;
        if (!space_.map_page(address, page_base, attributes)) {
            SetError(error, "the page tables could not be rebuilt");
            return false;
        }
    }
    region->permissions = permissions;
    error.clear();
    return true;
}

std::uint64_t KAddressSpace::CreateHeap(std::uint64_t size) {
    if (size == 0) {
        return 0;
    }
    const std::uint64_t base = (next_free_ + kPageMask) & ~kPageMask;
    const std::uint64_t rounded = (size + kPageMask) & ~kPageMask;
    if (base > kLastGuestAddress || rounded > kLastGuestAddress - base + 1) {
        return 0;
    }

    std::string error;
    if (!Reserve(base, rounded, error)) {
        return 0;
    }
    Region* region = FindContaining(base, rounded);
    if (region == nullptr) {
        return 0;
    }
    // A heap is usable immediately. Reserving without mapping would hand the
    // guest a range that faults on its first touch, which is not a heap.
    if (!Map(base, rounded, kMemoryRead | kMemoryWrite, error)) {
        return 0;
    }
    region->heap = true;
    return base;
}

const KAddressSpace::Region* KAddressSpace::Find(std::uint64_t address) const {
    for (const Region& region : regions_) {
        if (address >= region.base && address < region.base + region.size) {
            return &region;
        }
    }
    return nullptr;
}

std::size_t KAddressSpace::mapped_regions() const noexcept {
    std::size_t count = 0;
    for (const Region& region : regions_) {
        if (region.mapped) {
            ++count;
        }
    }
    return count;
}

}  // namespace zlong::service
