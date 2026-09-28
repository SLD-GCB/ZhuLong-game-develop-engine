// 烛龙 (ZhuLong) - guest physical address space.
//
// Physical memory is deliberately dumb: a list of regions, each RAM region
// backed by host memory, plus a bump allocator. Address translation lives one
// level up, in Mmu.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace zlong::ram {

enum class RegionKind : std::uint8_t {
    Ram,
    Reserved,
    Mmio,
};

struct PhysicalRegion {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    RegionKind kind = RegionKind::Ram;
    /// Whether guest code may be fetched from this region. Used by the flat
    /// (MMU-off) path to decide code-write invalidation.
    bool executable = false;
};

/// NVIDIA Tegra X1 DRAM base.
inline constexpr std::uint64_t kTegraDramBase = 0x8000'0000ULL;
/// Switch main memory: 4 GiB LPDDR4.
inline constexpr std::uint64_t kDefaultDramSize = 4ULL * 1024 * 1024 * 1024;

/// Alignment of the host backing store for guest RAM. Vulkan's
/// VK_EXT_external_memory_host requires an imported host pointer to satisfy
/// VkPhysicalDeviceExternalMemoryHostPropertiesEXT::minImportedHostPointerAlignment
/// (4 KiB class on NVIDIA), which a plain std::vector does not guarantee. We
/// over-align so any sane requirement is already met.
inline constexpr std::size_t kHostAlignment = 64 * 1024;

/// Frees a block obtained from AllocateAligned.
struct AlignedFree {
    void operator()(std::uint8_t* pointer) const noexcept;
};

using AlignedBuffer = std::unique_ptr<std::uint8_t, AlignedFree>;

/// Allocate `size` zero-filled bytes aligned to at least `alignment`.
AlignedBuffer AllocateAligned(std::size_t size, std::size_t alignment);

/// The default machine layout: one executable DRAM region at the Tegra base.
/// Everything else is unmapped for now (MMIO is a later milestone).
std::vector<PhysicalRegion> DefaultLayout(std::uint64_t dram_size = kDefaultDramSize);

class PhysicalMemory {
public:
    explicit PhysicalMemory(std::vector<PhysicalRegion> regions);

    PhysicalMemory(const PhysicalMemory&) = delete;
    PhysicalMemory& operator=(const PhysicalMemory&) = delete;

    const std::vector<PhysicalRegion>& regions() const noexcept { return regions_; }

    /// True when [pa, pa + n) lies entirely inside a RAM region.
    bool mapped(std::uint64_t pa, std::size_t n) const noexcept;
    bool is_executable(std::uint64_t pa) const noexcept;

    /// Bump-allocate `size` bytes with the given power-of-two alignment.
    /// Returns the guest physical address.
    ///
    /// The cursor starts at the base of the first RAM region and only moves up,
    /// so the bottom of DRAM is handed out first. The address space's translation
    /// tables come from here (see AddressSpace::enable), which means an image
    /// written to a *hard-coded* low physical address can land on a table and
    /// corrupt the walk -- silently, because the write itself succeeds. Load an
    /// image at an address this allocator returned, or reserve the low range
    /// first; do not assume it is free.
    std::optional<std::uint64_t> allocate(std::size_t size, std::size_t align = 0x1000);

    bool read(std::uint64_t pa, void* dst, std::size_t n) const;
    bool write(std::uint64_t pa, const void* src, std::size_t n);
    bool read64(std::uint64_t pa, std::uint64_t& out) const;
    bool write64(std::uint64_t pa, std::uint64_t value);

    /// Host pointer into guest RAM, or null when unmapped/not RAM. For the
    /// loader and for tests.
    std::uint8_t* host_pointer(std::uint64_t pa) noexcept;
    const std::uint8_t* host_pointer(std::uint64_t pa) const noexcept;

    std::size_t bytes_allocated() const noexcept { return allocated_; }

    /// The alignment guaranteed for every host_pointer() result. The GPU layer
    /// needs this to decide whether a direct host import is possible.
    static constexpr std::size_t host_alignment() noexcept { return kHostAlignment; }

private:
    struct Backing {
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        AlignedBuffer data;
    };

    Backing* find(std::uint64_t pa) noexcept;
    const Backing* find(std::uint64_t pa) const noexcept;

    std::vector<PhysicalRegion> regions_;
    // Built once in the constructor and never resized, so Backing* stays valid.
    std::vector<Backing> backings_;
    std::uint64_t next_alloc_ = 0;
    std::size_t allocated_ = 0;
};

}  // namespace zlong::ram
