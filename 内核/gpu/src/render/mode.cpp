#include "zlong/gpu/render/mode.h"

namespace zlong::gpu::render {

const char* ToString(RenderMode mode) noexcept {
    switch (mode) {
    case RenderMode::Auto:
        return "auto";
    case RenderMode::Vulkan:
        return "vulkan";
    case RenderMode::Software:
        return "software";
    }
    return "unknown";
}

const char* ToString(ActiveBackend backend) noexcept {
    switch (backend) {
    case ActiveBackend::None:
        return "none";
    case ActiveBackend::Vulkan:
        return "vulkan";
    case ActiveBackend::Software:
        return "software";
    }
    return "unknown";
}

namespace {

/// The Vulkan render backend renders through dynamic rendering and relies on
/// synchronization2, so both must be present for it to be usable.
bool VulkanUsable(const DeviceCapabilities& capabilities) noexcept {
    return capabilities.device_available && capabilities.dynamic_rendering &&
           capabilities.synchronization2;
}

}  // namespace

ModeDecision ChooseMode(RenderMode requested, const DeviceCapabilities& capabilities,
                        std::string vulkan_error) {
    ModeDecision decision;
    decision.requested = requested;
    decision.vulkan_error = std::move(vulkan_error);

    switch (requested) {
    case RenderMode::Software:
        decision.chosen = ActiveBackend::Software;
        decision.reason = "forced by caller";
        return decision;

    case RenderMode::Vulkan:
        if (!VulkanUsable(capabilities)) {
            // A forced mode must never silently degrade.
            decision.chosen = ActiveBackend::None;
            if (decision.vulkan_error.empty()) {
                decision.vulkan_error =
                    capabilities.device_available
                        ? "device lacks dynamic rendering / synchronization2"
                        : "no usable Vulkan device";
            }
            decision.reason = "Vulkan was forced but is unavailable: " + decision.vulkan_error;
            return decision;
        }
        decision.chosen = ActiveBackend::Vulkan;
        decision.reason = "forced by caller";
        return decision;

    case RenderMode::Auto:
        break;
    }

    if (VulkanUsable(capabilities)) {
        decision.chosen = ActiveBackend::Vulkan;
        decision.reason = "Auto: Vulkan device " + capabilities.device_name;
        return decision;
    }

    decision.chosen = ActiveBackend::Software;
    if (!capabilities.device_available) {
        decision.reason = "Auto: no Vulkan device (" +
                          (decision.vulkan_error.empty() ? std::string("unavailable")
                                                         : decision.vulkan_error) +
                          ")";
    } else {
        decision.reason = "Auto: device lacks dynamic rendering / synchronization2";
    }
    return decision;
}

}  // namespace zlong::gpu::render
