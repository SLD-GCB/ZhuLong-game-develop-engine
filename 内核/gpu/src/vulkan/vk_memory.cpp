#include "zlong/gpu/vulkan/vk_memory.h"

#include <cstring>

namespace zlong::gpu::vulkan {

namespace {

/// Usages an imported region buffer may need. A single imported buffer per RAM
/// region serves every caller, so the usage set is broad.
constexpr VkBufferUsageFlags kImportedUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

VkDeviceSize RoundUp(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

}  // namespace

VkMemory::VkMemory(VkContext& context, ram::PhysicalMemory& physical, Options options)
    : context_(context), physical_(physical) {
    import_alignment_ = context_.probe().min_import_alignment;
    if (import_alignment_ < 1) {
        import_alignment_ = 1;
    }

    command_pool_ =
        context_.CreateCommandPool(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    if (command_pool_ != VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(context_.device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
            fence_ = VK_NULL_HANDLE;
        }
    }

    GuestMemoryMode chosen = GuestMemoryMode::Mirrored;
    std::string reason;

    const bool usable = context_.probe().external_memory_host && command_pool_ != VK_NULL_HANDLE &&
                        fence_ != VK_NULL_HANDLE;
    if (!usable) {
        reason = context_.probe().external_memory_host
                     ? "memory layer could not create its command pool/fence"
                     : "VK_EXT_external_memory_host unavailable";
    } else {
        // Imported memory that only reaches host-visible (PCIe) types is not a
        // fast path, so prefer a device-local copy in that case.
        bool saw_region = false;
        bool device_local_importable = false;
        for (const auto& region : physical_.regions()) {
            if (region.kind != ram::RegionKind::Ram || region.size == 0) {
                continue;
            }
            saw_region = true;
            const auto* host = physical_.host_pointer(region.base);
            if (host == nullptr ||
                (reinterpret_cast<std::uintptr_t>(host) % import_alignment_) != 0) {
                continue;
            }
            const auto properties = context_.QueryHostPointer(host);
            if (!properties.supported) {
                continue;
            }
            if (context_.FindMemoryType(properties.memory_type_bits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != UINT32_MAX) {
                device_local_importable = true;
                break;
            }
        }

        if (!saw_region) {
            reason = "no RAM regions to import";
        } else if (device_local_importable) {
            chosen = GuestMemoryMode::Imported;
            reason = "host import reaches device-local memory";
        } else {
            reason = "host import only reaches host-visible (PCIe) memory; "
                     "using a device-local copy instead";
        }
    }

    if (options.force_mode.has_value()) {
        chosen = *options.force_mode;
        reason = "mode forced by caller";
    }

    mode_ = chosen;
    mode_reason_ = reason;

    if (mode_ == GuestMemoryMode::Imported && !TryImportRegions()) {
        mode_ = GuestMemoryMode::Mirrored;
        mode_reason_ = "host import failed at creation time; using a device-local copy";
    }
}

VkMemory::~VkMemory() {
    const VkDevice device = context_.device();
    for (auto& region : imported_) {
        if (region.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, region.memory, nullptr);
        }
        if (region.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, region.buffer, nullptr);
        }
    }
    imported_.clear();

    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
}

bool VkMemory::TryImportRegions() {
    const VkDevice device = context_.device();

    for (const auto& region : physical_.regions()) {
        if (region.kind != ram::RegionKind::Ram || region.size == 0) {
            continue;
        }
        auto* host = physical_.host_pointer(region.base);
        if (host == nullptr ||
            (reinterpret_cast<std::uintptr_t>(host) % import_alignment_) != 0) {
            continue;
        }
        const auto properties = context_.QueryHostPointer(host);
        if (!properties.supported) {
            continue;
        }

        std::uint32_t memory_type = context_.FindMemoryType(
            properties.memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type == UINT32_MAX) {
            memory_type = context_.FindMemoryType(properties.memory_type_bits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        }
        if (memory_type == UINT32_MAX) {
            for (std::uint32_t i = 0; i < 32; ++i) {
                if ((properties.memory_type_bits & (1u << i)) != 0) {
                    memory_type = i;
                    break;
                }
            }
        }
        if (memory_type == UINT32_MAX) {
            continue;
        }

        VkExternalMemoryBufferCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.pNext = &external;
        buffer_info.size = region.size;
        buffer_info.usage = kImportedUsage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        ImportedRegion imported;
        imported.base = region.base;
        imported.size = region.size;
        if (vkCreateBuffer(device, &buffer_info, nullptr, &imported.buffer) != VK_SUCCESS) {
            continue;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, imported.buffer, &requirements);

        VkDeviceSize allocation_size = requirements.size > region.size ? requirements.size
                                                                       : region.size;
        allocation_size =
            RoundUp(allocation_size, static_cast<VkDeviceSize>(import_alignment_));

        VkImportMemoryHostPointerInfoEXT import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = host;

        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &import;
        allocate.allocationSize = allocation_size;
        allocate.memoryTypeIndex = memory_type;

        if (vkAllocateMemory(device, &allocate, nullptr, &imported.memory) != VK_SUCCESS) {
            vkDestroyBuffer(device, imported.buffer, nullptr);
            continue;
        }
        if (vkBindBufferMemory(device, imported.buffer, imported.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(device, imported.memory, nullptr);
            vkDestroyBuffer(device, imported.buffer, nullptr);
            continue;
        }
        imported_.push_back(imported);
    }

    return !imported_.empty();
}

const VkMemory::ImportedRegion* VkMemory::FindRegion(GuestPa pa, std::uint64_t size) const {
    for (const auto& region : imported_) {
        if (pa < region.base) {
            continue;
        }
        const std::uint64_t offset = pa - region.base;
        if (offset <= region.size && size <= region.size - offset) {
            return &region;
        }
    }
    return nullptr;
}

std::optional<GuestBuffer> VkMemory::CreateBuffer(GuestPa pa, std::uint64_t size,
                                                  VkBufferUsageFlags usage) {
    if (size == 0 || !physical_.mapped(pa, static_cast<std::size_t>(size))) {
        return std::nullopt;
    }

    if (mode_ == GuestMemoryMode::Imported) {
        const ImportedRegion* region = FindRegion(pa, size);
        if (region == nullptr) {
            return std::nullopt;
        }
        GuestBuffer buffer;
        buffer.buffer = region->buffer;
        buffer.offset = pa - region->base;
        buffer.size = size;
        buffer.mirrored = false;
        buffer.owned = false;
        // Imported regions are created with the broad usage set already.
        (void)usage;
        return buffer;
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage =
        usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    const VkDevice device = context_.device();
    VkBuffer raw = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &raw) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, raw, &requirements);
    std::uint32_t memory_type = context_.FindMemoryType(requirements.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        memory_type = context_.FindMemoryType(requirements.memoryTypeBits, 0);
    }
    if (memory_type == UINT32_MAX) {
        vkDestroyBuffer(device, raw, nullptr);
        return std::nullopt;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, raw, nullptr);
        return std::nullopt;
    }
    if (vkBindBufferMemory(device, raw, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, raw, nullptr);
        return std::nullopt;
    }

    GuestBuffer buffer;
    buffer.buffer = raw;
    buffer.offset = 0;
    buffer.size = size;
    buffer.mirrored = true;
    buffer.owned = true;
    buffer.memory = memory;
    return buffer;
}

void VkMemory::DestroyBuffer(GuestBuffer& buffer) {
    if (buffer.owned) {
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(context_.device(), buffer.memory, nullptr);
        }
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(context_.device(), buffer.buffer, nullptr);
        }
    }
    buffer = GuestBuffer{};
}

bool VkMemory::CreateStaging(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                             void** mapped) const {
    const VkDevice device = context_.device();

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const std::uint32_t memory_type =
        context_.FindMemoryType(requirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        memory = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        memory = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VkMemory::DestroyStaging(VkBuffer buffer, VkDeviceMemory memory, void* mapped) const {
    const VkDevice device = context_.device();
    if (mapped != nullptr && memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, memory);
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
    }
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
    }
}

VkCommandBuffer VkMemory::BeginOneShot() const {
    if (command_pool_ == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = command_pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;

    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(context_.device(), &allocate, &commands) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        vkFreeCommandBuffers(context_.device(), command_pool_, 1, &commands);
        return VK_NULL_HANDLE;
    }
    return commands;
}

bool VkMemory::SubmitAndWait(VkCommandBuffer commands) {
    if (commands == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
        return false;
    }
    if (vkEndCommandBuffer(commands) != VK_SUCCESS) {
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;

    vkResetFences(context_.device(), 1, &fence_);
    if (vkQueueSubmit(context_.queue(), 1, &submit, fence_) != VK_SUCCESS) {
        return false;
    }
    return vkWaitForFences(context_.device(), 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

void VkMemory::EndOneShot(VkCommandBuffer commands) const {
    if (commands != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(context_.device(), command_pool_, 1, &commands);
    }
}

bool VkMemory::Upload(const GuestBuffer& buffer, GuestPa pa, std::uint64_t size) {
    if (!buffer.mirrored) {
        return true;  // imported: the device already sees the guest bytes
    }
    if (guest_ == nullptr) {
        return false;
    }
    const auto* source = guest_->host_pointer(pa, size);
    if (source == nullptr) {
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!CreateStaging(size, staging, staging_memory, &mapped)) {
        return false;
    }
    std::memcpy(mapped, source, static_cast<std::size_t>(size));

    bool ok = false;
    if (VkCommandBuffer commands = BeginOneShot(); commands != VK_NULL_HANDLE) {
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = buffer.offset;
        region.size = size;
        vkCmdCopyBuffer(commands, staging, buffer.buffer, 1, &region);
        ok = SubmitAndWait(commands);
        EndOneShot(commands);
    }

    DestroyStaging(staging, staging_memory, mapped);
    return ok;
}

bool VkMemory::Download(const GuestBuffer& buffer, GuestPa pa, std::uint64_t size) {
    if (!buffer.mirrored) {
        return true;  // imported: guest memory already holds the result
    }
    if (guest_ == nullptr) {
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!CreateStaging(size, staging, staging_memory, &mapped)) {
        return false;
    }

    bool ok = false;
    if (VkCommandBuffer commands = BeginOneShot(); commands != VK_NULL_HANDLE) {
        VkBufferCopy region{};
        region.srcOffset = buffer.offset;
        region.dstOffset = 0;
        region.size = size;
        vkCmdCopyBuffer(commands, buffer.buffer, staging, 1, &region);
        ok = SubmitAndWait(commands);
        EndOneShot(commands);
    }

    // Host-visible + host-coherent staging, so a plain read is enough.
    if (ok) {
        ok = guest_->GuestWrite(pa, mapped, static_cast<std::size_t>(size));
    }

    DestroyStaging(staging, staging_memory, mapped);
    return ok;
}

bool VkMemory::Fill(const GuestBuffer& buffer, std::uint32_t value) {
    if ((buffer.offset % 4) != 0 || (buffer.size % 4) != 0) {
        return false;
    }
    bool ok = false;
    if (VkCommandBuffer commands = BeginOneShot(); commands != VK_NULL_HANDLE) {
        vkCmdFillBuffer(commands, buffer.buffer, buffer.offset, buffer.size, value);
        ok = SubmitAndWait(commands);
        EndOneShot(commands);
    }
    return ok;
}

}  // namespace zlong::gpu::vulkan
