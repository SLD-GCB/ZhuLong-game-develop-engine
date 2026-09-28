// 烛龙 (ZhuLong) - the ONLY place a guest surface format meets VkFormat.

#pragma once

#include <vulkan/vulkan.h>

#include "zlong/gpu/surface/format.h"

namespace zlong::gpu::vulkan {

VkFormat ToVkFormat(surface::SurfaceFormat format) noexcept;

}  // namespace zlong::gpu::vulkan
