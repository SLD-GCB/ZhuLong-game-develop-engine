#include "zlong/ram/physical.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

// Windows' aligned allocator lives in <malloc.h> whatever the compiler is, not only under MSVC.
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace zlong::ram {

namespace {

void* AlignedAlloc(std::size_t size, std::size_t alignment) {
    if (size == 0) {
        return nullptr;
    }
// Which aligned allocator a platform has is a question about the platform, not about the compiler.
// This used to ask `_MSC_VER`, so any other compiler on Windows -- MinGW, clang -- fell into the
// `std::aligned_alloc` branch, and neither of them offers that from <cstdlib> on Windows. The pair
// also has to match: memory from `_aligned_malloc` must go back through `_aligned_free`.
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    // std::aligned_alloc requires size to be a multiple of alignment.
    const std::size_t rounded = ((size + alignment - 1) / alignment) * alignment;
    return std::aligned_alloc(alignment, rounded);
#endif
}

void AlignedDealloc(void* pointer) noexcept {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

}  // namespace

void AlignedFree::operator()(std::uint8_t* pointer) const noexcept {
    AlignedDealloc(pointer);
}

AlignedBuffer AllocateAligned(std::size_t size, std::size_t alignment) {
    void* raw = AlignedAlloc(size, alignment);
    if (raw == nullptr) {
        return AlignedBuffer{};
    }
    std::memset(raw, 0, size);
    return AlignedBuffer{static_cast<std::uint8_t*>(raw)};
}

std::vector<PhysicalRegion> DefaultLayout(std::uint64_t dram_size) {
    return {PhysicalRegion{kTegraDramBase, dram_size, RegionKind::Ram, /*executable=*/true}};
}

PhysicalMemory::PhysicalMemory(std::vector<PhysicalRegion> regions) : regions_(std::move(regions)) {
    backings_.reserve(regions_.size());
    for (const auto& region : regions_) {
        if (region.kind != RegionKind::Ram || region.size == 0) {
            continue;
        }
        Backing backing;
        backing.base = region.base;
        backing.size = region.size;
        backing.data = AllocateAligned(static_cast<std::size_t>(region.size), kHostAlignment);
        if (backing.data == nullptr) {
            throw std::bad_alloc();
        }
        backings_.push_back(std::move(backing));
    }
    // reserved, so Backing* handed out by find() stays valid for the lifetime of
    // this object.
    backings_.shrink_to_fit();
}

PhysicalMemory::Backing* PhysicalMemory::find(std::uint64_t pa) noexcept {
    for (auto& backing : backings_) {
        if (pa >= backing.base && pa < backing.base + backing.size) {
            return &backing;
        }
    }
    return nullptr;
}

const PhysicalMemory::Backing* PhysicalMemory::find(std::uint64_t pa) const noexcept {
    for (const auto& backing : backings_) {
        if (pa >= backing.base && pa < backing.base + backing.size) {
            return &backing;
        }
    }
    return nullptr;
}

bool PhysicalMemory::mapped(std::uint64_t pa, std::size_t n) const noexcept {
    if (n == 0) {
        return true;
    }
    const Backing* backing = find(pa);
    if (backing == nullptr) {
        return false;
    }
    const std::uint64_t offset = pa - backing->base;
    return n <= backing->size - offset;
}

bool PhysicalMemory::is_executable(std::uint64_t pa) const noexcept {
    for (const auto& region : regions_) {
        if (region.kind != RegionKind::Ram) {
            continue;
        }
        if (pa >= region.base && pa < region.base + region.size) {
            return region.executable;
        }
    }
    return false;
}

std::optional<std::uint64_t> PhysicalMemory::allocate(std::size_t size, std::size_t align) {
    if (size == 0) {
        size = 1;
    }
    if (align < 16) {
        align = 16;
    }
    for (const auto& region : regions_) {
        if (region.kind != RegionKind::Ram) {
            continue;
        }
        std::uint64_t cursor = next_alloc_ > region.base ? next_alloc_ : region.base;
        cursor = ((cursor + align - 1) / align) * align;
        if (cursor < region.base) {
            cursor = region.base;
        }
        if (cursor > region.base + region.size) {
            continue;
        }
        if (size <= region.base + region.size - cursor) {
            next_alloc_ = cursor + size;
            allocated_ += size;
            return cursor;
        }
    }
    return std::nullopt;
}

bool PhysicalMemory::read(std::uint64_t pa, void* dst, std::size_t n) const {
    const Backing* backing = find(pa);
    if (backing == nullptr) {
        return false;
    }
    const std::uint64_t offset = pa - backing->base;
    if (n > backing->size - offset) {
        return false;
    }
    std::memcpy(dst, backing->data.get() + offset, n);
    return true;
}

bool PhysicalMemory::write(std::uint64_t pa, const void* src, std::size_t n) {
    Backing* backing = find(pa);
    if (backing == nullptr) {
        return false;
    }
    const std::uint64_t offset = pa - backing->base;
    if (n > backing->size - offset) {
        return false;
    }
    std::memcpy(backing->data.get() + offset, src, n);
    return true;
}

bool PhysicalMemory::read64(std::uint64_t pa, std::uint64_t& out) const {
    return read(pa, &out, sizeof(out));
}

bool PhysicalMemory::write64(std::uint64_t pa, std::uint64_t value) {
    return write(pa, &value, sizeof(value));
}

std::uint8_t* PhysicalMemory::host_pointer(std::uint64_t pa) noexcept {
    Backing* backing = find(pa);
    if (backing == nullptr) {
        return nullptr;
    }
    return backing->data.get() + (pa - backing->base);
}

const std::uint8_t* PhysicalMemory::host_pointer(std::uint64_t pa) const noexcept {
    const Backing* backing = find(pa);
    if (backing == nullptr) {
        return nullptr;
    }
    return backing->data.get() + (pa - backing->base);
}

}  // namespace zlong::ram
