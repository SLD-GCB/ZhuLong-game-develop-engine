#include "zlong/gpu/memory/gpu_address_space.h"

namespace zlong::gpu {

GpuVAddr GpuAddressSpace::Map(GuestPa physical, std::uint64_t size) {
    for (const Mapping& mapping : mappings_) {
        if (mapping.physical == physical && mapping.size == size) {
            return mapping.address;
        }
    }

    Mapping mapping;
    mapping.address = next_;
    mapping.physical = physical;
    mapping.size = size;
    mappings_.push_back(mapping);
    next_ += size;
    return mapping.address;
}

std::optional<GuestPa> GpuAddressSpace::Resolve(GpuVAddr address, std::uint64_t size) const {
    // One mapping has to cover the whole range. A range that straddles two mappings is
    // refused rather than resolved piecewise: nothing submits across a mapping
    // boundary today, and guessing here would hide it if something did.
    for (const Mapping& mapping : mappings_) {
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
            mappings_.erase(it);
            return true;
        }
    }
    return false;
}

void GpuAddressSpace::Clear() {
    mappings_.clear();
    // The counter is deliberately not rewound: an address that was handed out once
    // should not come back as a different range's name.
}

}  // namespace zlong::gpu
