#include "zlong/gpu/memory/gpu_address_space.h"

#include <algorithm>
#include <limits>

namespace zlong::gpu {

namespace {

/// Round a size up to a whole number of pages. Zero for zero, and zero for a size
/// whose rounding would wrap -- in both cases there is no allocation to make.
std::uint64_t AlignUpPages(std::uint64_t size) {
    if (size == 0) {
        return 0;
    }
    const std::uint64_t remainder = size & GpuAddressSpace::kPageMask;
    if (remainder == 0) {
        return size;
    }
    const std::uint64_t padding = GpuAddressSpace::kPageSize - remainder;
    if (size > std::numeric_limits<std::uint64_t>::max() - padding) {
        return 0;
    }
    return size + padding;
}

}  // namespace

// ---------------------------------------------------------------------------
// processes
// ---------------------------------------------------------------------------

Asid GpuAddressSpace::RegisterProcess() {
    // Ids start at one and only go up. A wrap would hand out an id a live process
    // already holds, which is exactly the confusion the never-reused rule exists to
    // prevent -- so skip the zero rather than wrapping into it.
    if (next_asid_ == 0) {
        ++next_asid_;
    }
    return Asid{next_asid_++};
}

std::size_t GpuAddressSpace::UnregisterProcess(Asid asid) {
    if (!asid.valid()) {
        return 0;
    }

    std::size_t dropped = 0;
    for (auto it = mappings_.begin(); it != mappings_.end();) {
        if (it->asid == asid) {
            DropPageCounts(*it);
            it = mappings_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

// ---------------------------------------------------------------------------
// the device allocator
// ---------------------------------------------------------------------------

std::optional<GpuVAddr> GpuAddressSpace::Allocate(std::uint64_t size, Asid asid) {
    const std::uint64_t aligned = AlignUpPages(size);
    if (aligned == 0) {
        return std::nullopt;
    }

    const std::optional<GpuVAddr> address = FindGap(aligned);
    if (!address.has_value()) {
        return std::nullopt;
    }

    Mapping mapping;
    mapping.address = *address;
    mapping.size = aligned;
    mapping.asid = asid;
    mapping.backed = false;

    const auto position = std::lower_bound(
        mappings_.begin(), mappings_.end(), *address,
        [](const Mapping& m, GpuVAddr a) { return m.address < a; });
    mappings_.insert(position, mapping);
    return address;
}

bool GpuAddressSpace::AllocateFixed(GpuVAddr address, std::uint64_t size, Asid asid) {
    const std::uint64_t aligned = AlignUpPages(size);
    if (aligned == 0) {
        return false;
    }
    // A fixed address has to be where the caller said. Rounding it would be a
    // different allocation than the one that was asked for.
    if ((address & kPageMask) != 0) {
        return false;
    }
    if (address < kBase) {
        return false;
    }
    if (aligned > std::numeric_limits<GpuVAddr>::max() - address) {
        return false;
    }

    for (const Mapping& mapping : mappings_) {
        if (Overlaps(mapping, address, aligned)) {
            return false;
        }
    }

    Mapping mapping;
    mapping.address = address;
    mapping.size = aligned;
    mapping.asid = asid;
    mapping.backed = false;

    const auto position = std::lower_bound(
        mappings_.begin(), mappings_.end(), address,
        [](const Mapping& m, GpuVAddr a) { return m.address < a; });
    mappings_.insert(position, mapping);
    return true;
}

bool GpuAddressSpace::Free(GpuVAddr address, std::uint64_t size) {
    for (auto it = mappings_.begin(); it != mappings_.end(); ++it) {
        if (it->address != address) {
            continue;
        }
        // The size has to be the one that was handed out. Freeing a sub-range would
        // either drop mappings the caller still holds or leave a remainder nobody
        // described, so it is refused rather than split.
        if (it->size != size) {
            return false;
        }
        DropPageCounts(*it);
        mappings_.erase(it);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// naming physical memory
// ---------------------------------------------------------------------------

GpuVAddr GpuAddressSpace::Map(GuestPa physical, std::uint64_t size, Asid asid) {
    for (const Mapping& mapping : mappings_) {
        if (mapping.backed && mapping.physical == physical && mapping.size == size) {
            return mapping.address;
        }
    }

    const std::optional<GpuVAddr> address = Allocate(size, asid);
    if (!address.has_value()) {
        // Zero is not a GPU address -- the space starts at kBase and only goes up --
        // so it can report "no address" without being mistaken for one.
        return 0;
    }

    if (!Bind(*address, physical, size)) {
        // The range was reserved and bound in the same call, so a bind that fails
        // here means the reservation was not what Allocate just made. Give the
        // address back rather than leaving a reservation nobody can name.
        Free(*address, AlignUpPages(size));
        return 0;
    }
    return *address;
}

bool GpuAddressSpace::Bind(GpuVAddr address, GuestPa physical, std::uint64_t size) {
    Mapping* mapping = FindCovering(address, size);
    if (mapping == nullptr) {
        return false;
    }
    if (mapping->backed) {
        // Already names memory. Rebinding would move a range out from under whatever
        // is already using the address, which is a caller error, not a remap.
        return false;
    }

    mapping->physical = physical;
    mapping->backed = true;
    return true;
}

std::optional<GuestPa> GpuAddressSpace::Resolve(GpuVAddr address, std::uint64_t size) const {
    // One mapping has to cover the whole range. A range that straddles two mappings is
    // refused rather than resolved piecewise: nothing submits across a mapping
    // boundary today, and guessing here would hide it if something did.
    for (const Mapping& mapping : mappings_) {
        if (!mapping.backed) {
            continue;
        }
        if (address < mapping.address) {
            continue;
        }
        const std::uint64_t offset = address - mapping.address;
        if (offset <= mapping.size && size <= mapping.size - offset) {
            return mapping.physical + offset;
        }
    }
    return std::nullopt;
}

std::optional<GuestPa> GpuAddressSpace::Resolve(GpuVAddr address) const {
    for (const Mapping& mapping : mappings_) {
        if (!mapping.backed) {
            continue;
        }
        if (address < mapping.address) {
            continue;
        }
        const std::uint64_t offset = address - mapping.address;
        if (offset < mapping.size) {
            return mapping.physical + offset;
        }
    }
    return std::nullopt;
}

std::optional<GpuVAddr> GpuAddressSpace::AddressOf(GuestPa physical, std::uint64_t size) const {
    for (const Mapping& mapping : mappings_) {
        if (!mapping.backed) {
            continue;
        }
        if (physical < mapping.physical) {
            continue;
        }
        const std::uint64_t offset = physical - mapping.physical;
        if (offset <= mapping.size && size <= mapping.size - offset) {
            return mapping.address + offset;
        }
    }
    return std::nullopt;
}

std::optional<GpuVAddr> GpuAddressSpace::AddressOf(GuestPa physical) const {
    for (const Mapping& mapping : mappings_) {
        if (!mapping.backed) {
            continue;
        }
        if (physical < mapping.physical) {
            continue;
        }
        const std::uint64_t offset = physical - mapping.physical;
        if (offset < mapping.size) {
            return mapping.address + offset;
        }
    }
    return std::nullopt;
}

bool GpuAddressSpace::Unmap(GpuVAddr address) {
    for (auto it = mappings_.begin(); it != mappings_.end(); ++it) {
        if (it->address == address) {
            DropPageCounts(*it);
            mappings_.erase(it);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// cached-page accounting
// ---------------------------------------------------------------------------

void GpuAddressSpace::UpdateCachedCount(GpuVAddr address, std::uint64_t size, int delta) {
    if (delta == 0 || size == 0) {
        return;
    }

    std::uint64_t remaining = size;
    GpuVAddr cursor = address;

    while (remaining > 0) {
        const std::uint64_t in_page = cursor & kPageMask;
        const std::uint64_t chunk = std::min<std::uint64_t>(kPageSize - in_page, remaining);

        // A range that names nothing has no page to count, and a cache saying it
        // dropped one it held is not a fault -- so an unmapped page is skipped, not
        // refused.
        if (const std::optional<GuestPa> physical = Resolve(cursor, chunk)) {
            const GpuVAddr page = cursor & ~kPageMask;
            std::int32_t& count = cached_pages_[page];
            const std::int32_t before = count;

            // Clamped at zero: a cache that reports more drops than additions is
            // wrong somewhere, and a negative count would keep a page permanently
            // "cached" once it had been miscounted.
            const std::int64_t moved = static_cast<std::int64_t>(before) + delta;
            count = static_cast<std::int32_t>(
                std::clamp<std::int64_t>(moved, 0, std::numeric_limits<std::int32_t>::max()));

            if (cached_observer_ != nullptr) {
                // The page's own physical address, not the address the chunk started
                // at: the count is per page, so the notification has to be too.
                const GuestPa physical_page = *physical - in_page;
                if (before == 0 && count > 0) {
                    cached_observer_->OnPageCached(physical_page);
                } else if (before > 0 && count == 0) {
                    cached_observer_->OnPageUncached(physical_page);
                }
            }

            if (count == 0) {
                cached_pages_.erase(page);
            }
        }

        remaining -= chunk;
        cursor += chunk;
    }
}

void GpuAddressSpace::UpdateCachedBatch(std::span<const CachedRange> ranges, int delta) {
    for (const CachedRange& range : ranges) {
        UpdateCachedCount(range.address, range.size, delta);
    }
}

std::int32_t GpuAddressSpace::cached_count(GpuVAddr address) const {
    const auto it = cached_pages_.find(address & ~kPageMask);
    return it == cached_pages_.end() ? 0 : it->second;
}

// ---------------------------------------------------------------------------
// everything at once
// ---------------------------------------------------------------------------

void GpuAddressSpace::Clear() {
    mappings_.clear();
    cached_pages_.clear();
}

std::size_t GpuAddressSpace::mapping_count(Asid asid) const {
    std::size_t count = 0;
    for (const Mapping& mapping : mappings_) {
        if (mapping.asid == asid) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

bool GpuAddressSpace::Overlaps(const Mapping& mapping, GpuVAddr address, std::uint64_t size) {
    const GpuVAddr mapping_end = mapping.address + mapping.size;
    const GpuVAddr range_end = address + size;
    return mapping.address < range_end && address < mapping_end;
}

const GpuAddressSpace::Mapping* GpuAddressSpace::FindCovering(GpuVAddr address,
                                                              std::uint64_t size) const {
    for (const Mapping& mapping : mappings_) {
        if (address < mapping.address) {
            continue;
        }
        const std::uint64_t offset = address - mapping.address;
        if (offset <= mapping.size && size <= mapping.size - offset) {
            return &mapping;
        }
    }
    return nullptr;
}

GpuAddressSpace::Mapping* GpuAddressSpace::FindCovering(GpuVAddr address, std::uint64_t size) {
    for (Mapping& mapping : mappings_) {
        if (address < mapping.address) {
            continue;
        }
        const std::uint64_t offset = address - mapping.address;
        if (offset <= mapping.size && size <= mapping.size - offset) {
            return &mapping;
        }
    }
    return nullptr;
}

std::optional<GpuVAddr> GpuAddressSpace::FindGap(std::uint64_t size) const {
    if (size == 0) {
        return std::nullopt;
    }

    // The list is ordered, so the first gap is found in one pass: keep a cursor at
    // the end of everything seen so far and stop as soon as a mapping starts far
    // enough above it.
    GpuVAddr cursor = kBase;
    for (const Mapping& mapping : mappings_) {
        if (mapping.address > cursor && mapping.address - cursor >= size) {
            return cursor;
        }
        const GpuVAddr end = mapping.address + mapping.size;
        if (end < mapping.address) {
            // The mapping wrapped, so there is no trustworthy end to advance to.
            return std::nullopt;
        }
        if (end > cursor) {
            cursor = end;
        }
    }

    if (size > std::numeric_limits<GpuVAddr>::max() - cursor) {
        return std::nullopt;
    }
    return cursor;
}

void GpuAddressSpace::DropPageCounts(const Mapping& mapping) {
    // Counts under a range that is going away are erased without a notification:
    // nothing is being uncached, the range is simply gone, and a renderer told it
    // had lost a page would look for a cache entry that no longer has an address.
    if (cached_pages_.empty()) {
        return;
    }
    const GpuVAddr end = mapping.address + mapping.size;
    cached_pages_.erase(cached_pages_.lower_bound(mapping.address),
                        cached_pages_.lower_bound(end));
}

}  // namespace zlong::gpu
