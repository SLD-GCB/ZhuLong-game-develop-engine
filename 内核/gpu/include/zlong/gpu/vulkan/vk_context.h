// 烛龙 (ZhuLong) - Vulkan instance/device/queue plus the first-run capability
// probe.
//
// Nothing in this layer assumes the host GPU can do anything: whether host
// memory can be imported, and with what alignment, is established by the probe
// at startup.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace zlong::gpu::vulkan {

struct DeviceInfo {
    std::string name;
    std::uint32_t api_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    bool is_discrete = false;
};

/// Results of the capability probe. The memory plan depends on these, so they
/// are measured rather than assumed.
struct Probe {
    /// VK_EXT_external_memory_host available, i.e. host memory can be imported.
    bool external_memory_host = false;
    /// minImportedHostPointerAlignment (1 when unknown).
    std::uint64_t min_import_alignment = 1;
    bool timeline_semaphore = false;
    bool dynamic_rendering = false;
    bool synchronization2 = false;
};

struct HostPointerProperties {
    bool supported = false;
    std::uint32_t memory_type_bits = 0;
};

class VkContext {
public:
    ~VkContext();

    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    /// Create instance + device + queue and run the probe. Returns nullptr and
    /// fills `error` when there is no usable Vulkan device.
    static std::unique_ptr<VkContext> Create(bool enable_validation, std::string& error);

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physical_device() const noexcept { return physical_; }
    VkDevice device() const noexcept { return device_; }
    VkQueue queue() const noexcept { return queue_; }
    std::uint32_t queue_family() const noexcept { return queue_family_; }

    const DeviceInfo& info() const noexcept { return info_; }
    const Probe& probe() const noexcept { return probe_; }
    const VkPhysicalDeviceProperties& properties() const noexcept { return properties_; }

    /// Which memory types can import `host_pointer`.
    HostPointerProperties QueryHostPointer(const void* host_pointer) const;

    VkCommandPool CreateCommandPool(VkCommandPoolCreateFlags flags = 0) const;

    /// First memory type in `type_bits` satisfying all `flags`, or UINT32_MAX.
    std::uint32_t FindMemoryType(std::uint32_t type_bits, VkMemoryPropertyFlags flags) const;

private:
    VkContext() = default;
    void Destroy();
    void CreateDebugMessenger();
    void DestroyDebugMessenger();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties properties_{};
    DeviceInfo info_{};
    Probe probe_{};

    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties_ = nullptr;
};

}  // namespace zlong::gpu::vulkan
