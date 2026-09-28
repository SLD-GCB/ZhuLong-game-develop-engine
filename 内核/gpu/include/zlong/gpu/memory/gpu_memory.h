// 烛龙 (ZhuLong) - guest RAM access for the GPU side.
//
// This is the one place that routes guest memory writes made by the GPU.
// PhysicalMemory::write deliberately does not notify the code-write observer
// (only Ram::write does), so a GPU write that lands in executable guest memory
// would otherwise leave stale JIT translations behind.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "zlong/cpu/memory.h"
#include "zlong/gpu/resource.h"
#include "zlong/gpu/types.h"
#include "zlong/ram/physical.h"

namespace zlong::gpu {

class GpuMemoryManager : public MemorySource {
public:
    explicit GpuMemoryManager(ram::PhysicalMemory& physical);

    GpuMemoryManager(const GpuMemoryManager&) = delete;
    GpuMemoryManager& operator=(const GpuMemoryManager&) = delete;

    void set_code_observer(cpu::CodeWriteObserver* observer) noexcept { observer_ = observer; }
    cpu::CodeWriteObserver* code_observer() const noexcept { return observer_; }

    /// Ranges occupied by GPU page tables. A GPU write into such a range can
    /// change translations anywhere, so it flushes every JIT cache. Defaults to
    /// "no page tables", which the GPU address space registers later.
    using PageTablePredicate = std::function<bool(GuestPa pa, std::size_t n)>;
    void set_page_table_predicate(PageTablePredicate predicate) { page_table_ = std::move(predicate); }

    ram::PhysicalMemory& physical() noexcept { return physical_; }
    const ram::PhysicalMemory& physical() const noexcept { return physical_; }

    /// Host pointer for a fully mapped range, or nullptr when any part is not
    /// RAM.
    std::uint8_t* host_pointer(GuestPa pa, std::uint64_t size) const;

    /// MemorySource: read-only view for the shader interpreter and the software
    /// renderer.
    const std::uint8_t* peek(GuestPa pa, std::uint64_t size) const override {
        return host_pointer(pa, size);
    }

    bool GuestRead(GuestPa pa, void* dst, std::size_t n) const;

    /// Write into guest RAM, then report the write to the code-write observer
    /// when it matters (executable memory, or page-table memory).
    bool GuestWrite(GuestPa pa, const void* src, std::size_t n);

private:
    ram::PhysicalMemory& physical_;
    cpu::CodeWriteObserver* observer_ = nullptr;
    PageTablePredicate page_table_;
};

}  // namespace zlong::gpu
