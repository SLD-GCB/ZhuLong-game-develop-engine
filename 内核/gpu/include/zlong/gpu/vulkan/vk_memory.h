// 烛龙 (ZhuLong) - guest RAM as seen by Vulkan.
//
// The Switch is a unified-memory machine: there is no separate VRAM, so "显存" is
// a subset of guest physical memory. This class therefore tries to alias the
// existing host allocation (VK_EXT_external_memory_host, one physical copy,
// coherent with the CPU for free) and falls back to a device-local copy when the
// host can only offer PCIe-visible memory.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "zlong/gpu/memory/gpu_memory.h"
#include "zlong/gpu/types.h"
#include "zlong/gpu/vulkan/vk_context.h"
#include "zlong/ram/physical.h"

namespace zlong::gpu::vulkan {

enum class GuestMemoryMode : std::uint8_t {
    /// The VkBuffer aliases the guest host allocation.
    Imported,
    /// A device-local copy plus explicit upload/download around GPU work.
    Mirrored,
};

/// A window onto a guest memory range.
struct GuestBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    /// Offset of the guest range inside `buffer`.
    VkDeviceSize offset = 0;
    /// Size of the guest range.
    VkDeviceSize size = 0;
    /// Contents are a device-local copy that must be uploaded/downloaded.
    bool mirrored = false;
    /// This object owns `buffer`/`memory` and must destroy them.
    bool owned = false;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

class VkMemory {
public:
    struct Options {
        /// Tests pin a mode; by default it is chosen from the capability probe.
        std::optional<GuestMemoryMode> force_mode;
    };

    VkMemory(VkContext& context, ram::PhysicalMemory& physical, Options options = {});
    ~VkMemory();

    VkMemory(const VkMemory&) = delete;
    VkMemory& operator=(const VkMemory&) = delete;

    GuestMemoryMode mode() const noexcept { return mode_; }
    /// Why this mode was chosen. Worth asserting on: on a discrete GPU the
    /// importable memory may be host-visible only, which is not a fast path.
    const std::string& mode_reason() const noexcept { return mode_reason_; }
    std::size_t imported_regions() const noexcept { return imported_.size(); }

    void set_memory_manager(GpuMemoryManager* memory) noexcept { guest_ = memory; }

    /// A buffer covering [pa, pa + size). Imported mode returns a window into the
    /// region's imported buffer; mirrored mode allocates a device-local one.
    std::optional<GuestBuffer> CreateBuffer(GuestPa pa, std::uint64_t size, VkBufferUsageFlags usage);
    void DestroyBuffer(GuestBuffer& buffer);

    /// Make the device copy match guest RAM. No-op when imported.
    bool Upload(const GuestBuffer& buffer, GuestPa pa, std::uint64_t size);

    /// Make guest RAM match the device copy (routed through
    /// GpuMemoryManager::GuestWrite). No-op when imported.
    bool Download(const GuestBuffer& buffer, GuestPa pa, std::uint64_t size);

    /// Fill the buffer's guest range with `value` on the GPU (vkCmdFillBuffer).
    /// Offset and size must be multiples of 4.
    bool Fill(const GuestBuffer& buffer, std::uint32_t value);

private:
    struct ImportedRegion {
        GuestPa base = 0;
        std::uint64_t size = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool TryImportRegions();
    const ImportedRegion* FindRegion(GuestPa pa, std::uint64_t size) const;

    bool CreateStaging(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                       void** mapped) const;
    void DestroyStaging(VkBuffer buffer, VkDeviceMemory memory, void* mapped) const;

    VkCommandBuffer BeginOneShot() const;
    bool SubmitAndWait(VkCommandBuffer commands);
    void EndOneShot(VkCommandBuffer commands) const;

    VkContext& context_;
    ram::PhysicalMemory& physical_;
    GpuMemoryManager* guest_ = nullptr;

    GuestMemoryMode mode_ = GuestMemoryMode::Mirrored;
    std::string mode_reason_;
    std::uint64_t import_alignment_ = 1;
    std::vector<ImportedRegion> imported_;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

}  // namespace zlong::gpu::vulkan
