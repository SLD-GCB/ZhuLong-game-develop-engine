#include "zlong/gpu/vulkan/vk_context.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace zlong::gpu::vulkan {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kExternalMemoryHostExtension = "VK_EXT_external_memory_host";

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*user*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool HasLayer(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    std::vector<VkLayerProperties> available(count);
    if (vkEnumerateInstanceLayerProperties(&count, available.data()) != VK_SUCCESS) {
        return false;
    }
    for (const auto& layer : available) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool HasDeviceExtension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return false;
    }
    std::vector<VkExtensionProperties> available(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data()) !=
        VK_SUCCESS) {
        return false;
    }
    for (const auto& extension : available) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Prefer a discrete GPU, then an integrated one.
int ScoreDevice(const VkPhysicalDeviceProperties& properties) {
    switch (properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 100;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 10;
    default:
        return 1;
    }
}

}  // namespace

VkContext::~VkContext() {
    Destroy();
}

void VkContext::CreateDebugMessenger() {
    if (instance_ == VK_NULL_HANDLE) {
        return;
    }
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (create == nullptr) {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = &DebugCallback;
    create(instance_, &info, nullptr, &debug_messenger_);
}

void VkContext::DestroyDebugMessenger() {
    if (debug_messenger_ == VK_NULL_HANDLE || instance_ == VK_NULL_HANDLE) {
        return;
    }
    const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy != nullptr) {
        destroy(instance_, debug_messenger_, nullptr);
    }
    debug_messenger_ = VK_NULL_HANDLE;
}

void VkContext::Destroy() {
    DestroyDebugMessenger();
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

std::unique_ptr<VkContext> VkContext::Create(bool enable_validation, std::string& error) {
    std::unique_ptr<VkContext> context(new VkContext());

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "ZhuLong";
    application.pEngineName = "ZhuLong";
    application.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> instance_extensions;
    if (enable_validation && HasLayer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames =
        instance_extensions.empty() ? nullptr : instance_extensions.data();

    if (vkCreateInstance(&instance_info, nullptr, &context->instance_) != VK_SUCCESS) {
        error = "vkCreateInstance failed (no Vulkan loader or ICD?)";
        return nullptr;
    }
    if (!layers.empty()) {
        context->CreateDebugMessenger();
    }

    std::uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(context->instance_, &device_count, nullptr) != VK_SUCCESS ||
        device_count == 0) {
        error = "no Vulkan physical devices";
        context->Destroy();
        return nullptr;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(context->instance_, &device_count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    std::uint32_t best_family = 0;
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
        if (family_count == 0) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());

        std::uint32_t graphics = UINT32_MAX;
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                graphics = i;
                break;
            }
        }
        if (graphics == UINT32_MAX) {
            continue;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        const int score = ScoreDevice(properties);
        if (score > best_score) {
            best_score = score;
            best = device;
            best_family = graphics;
        }
    }
    if (best == VK_NULL_HANDLE) {
        error = "no Vulkan device exposes a graphics queue";
        context->Destroy();
        return nullptr;
    }

    context->physical_ = best;
    context->queue_family_ = best_family;
    vkGetPhysicalDeviceProperties(best, &context->properties_);
    context->info_.name = context->properties_.deviceName;
    context->info_.api_version = context->properties_.apiVersion;
    context->info_.vendor_id = context->properties_.vendorID;
    context->info_.device_id = context->properties_.deviceID;
    context->info_.is_discrete =
        context->properties_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    // Enable only features the device actually reports.
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    supported13.pNext = &supported12;
    VkPhysicalDeviceFeatures2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported.pNext = &supported13;
    vkGetPhysicalDeviceFeatures2(best, &supported);

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = supported12.timelineSemaphore;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = &features12;
    features13.dynamicRendering = supported13.dynamicRendering;
    features13.synchronization2 = supported13.synchronization2;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;

    std::vector<const char*> device_extensions;
    const bool has_external_memory_host = HasDeviceExtension(best, kExternalMemoryHostExtension);
    if (has_external_memory_host) {
        device_extensions.push_back(kExternalMemoryHostExtension);
    }

    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = best_family;
    queue_info.queueCount = 1;
    const float priority = 1.0f;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames =
        device_extensions.empty() ? nullptr : device_extensions.data();

    if (vkCreateDevice(best, &device_info, nullptr, &context->device_) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        context->Destroy();
        return nullptr;
    }
    vkGetDeviceQueue(context->device_, best_family, 0, &context->queue_);

    context->probe_.timeline_semaphore = features12.timelineSemaphore == VK_TRUE;
    context->probe_.dynamic_rendering = features13.dynamicRendering == VK_TRUE;
    context->probe_.synchronization2 = features13.synchronization2 == VK_TRUE;

    if (has_external_memory_host) {
        context->get_host_pointer_properties_ =
            reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                vkGetDeviceProcAddr(context->device_, "vkGetMemoryHostPointerPropertiesEXT"));

        if (context->get_host_pointer_properties_ != nullptr) {
            // minImportedHostPointerAlignment arrives through the
            // VkPhysicalDeviceProperties2 pNext chain; this SDK's headers
            // declare no dedicated query function for it.
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties{};
            host_properties.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &host_properties;
            vkGetPhysicalDeviceProperties2(best, &properties2);

            context->probe_.external_memory_host = true;
            if (host_properties.minImportedHostPointerAlignment > 0) {
                context->probe_.min_import_alignment =
                    host_properties.minImportedHostPointerAlignment;
            }
        }
    }

    error.clear();
    return context;
}

HostPointerProperties VkContext::QueryHostPointer(const void* host_pointer) const {
    HostPointerProperties out;
    if (get_host_pointer_properties_ == nullptr || host_pointer == nullptr) {
        return out;
    }
    VkMemoryHostPointerPropertiesEXT properties{};
    properties.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (get_host_pointer_properties_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                     host_pointer, &properties) != VK_SUCCESS) {
        return out;
    }
    out.supported = properties.memoryTypeBits != 0;
    out.memory_type_bits = properties.memoryTypeBits;
    return out;
}

VkCommandPool VkContext::CreateCommandPool(VkCommandPoolCreateFlags flags) const {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = flags;
    info.queueFamilyIndex = queue_family_;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device_, &info, nullptr, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pool;
}

std::uint32_t VkContext::FindMemoryType(std::uint32_t type_bits,
                                        VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        if ((memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

}  // namespace zlong::gpu::vulkan
