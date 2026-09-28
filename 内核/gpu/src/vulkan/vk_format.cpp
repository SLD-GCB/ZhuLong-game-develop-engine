#include "zlong/gpu/vulkan/vk_format.h"

namespace zlong::gpu::vulkan {

VkFormat ToVkFormat(surface::SurfaceFormat format) noexcept {
    switch (format) {
    case surface::SurfaceFormat::R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case surface::SurfaceFormat::R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case surface::SurfaceFormat::B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case surface::SurfaceFormat::A8R8G8B8_UNORM:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case surface::SurfaceFormat::R5G6B5_UNORM:
        return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case surface::SurfaceFormat::R4G4B4A4_UNORM:
        return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case surface::SurfaceFormat::R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case surface::SurfaceFormat::R32G32B32A32_FLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case surface::SurfaceFormat::R32G32_FLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case surface::SurfaceFormat::D16_UNORM:
        return VK_FORMAT_D16_UNORM;
    case surface::SurfaceFormat::D24S8_UNORM:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case surface::SurfaceFormat::D32_FLOAT:
        return VK_FORMAT_D32_SFLOAT;
    case surface::SurfaceFormat::Unknown:
        break;
    }
    return VK_FORMAT_UNDEFINED;
}

}  // namespace zlong::gpu::vulkan
