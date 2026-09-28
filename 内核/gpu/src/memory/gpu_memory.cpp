#include "zlong/gpu/memory/gpu_memory.h"

namespace zlong::gpu {

GpuMemoryManager::GpuMemoryManager(ram::PhysicalMemory& physical) : physical_(physical) {}

std::uint8_t* GpuMemoryManager::host_pointer(GuestPa pa, std::uint64_t size) const {
    if (size == 0 || !physical_.mapped(pa, static_cast<std::size_t>(size))) {
        return nullptr;
    }
    return physical_.host_pointer(pa);
}

bool GpuMemoryManager::GuestRead(GuestPa pa, void* dst, std::size_t n) const {
    if (n == 0) {
        return true;
    }
    return physical_.mapped(pa, n) && physical_.read(pa, dst, n);
}

bool GpuMemoryManager::GuestWrite(GuestPa pa, const void* src, std::size_t n) {
    if (n == 0) {
        return true;
    }
    if (!physical_.write(pa, src, n)) {
        return false;
    }

    if (observer_ != nullptr) {
        // Self-modifying code: the JIT may already hold a translation of what we
        // just overwrote.
        if (physical_.is_executable(pa)) {
            observer_->OnGuestCodeWrite(pa, n);
        }
        // Page tables live in guest memory; changing them can change
        // permissions or executability anywhere, so nothing can be trusted.
        if (page_table_ && page_table_(pa, n)) {
            observer_->OnGuestCodeInvalidateAll();
        }
    }
    return true;
}

}  // namespace zlong::gpu
