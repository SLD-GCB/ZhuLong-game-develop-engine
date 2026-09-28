// 烛龙 (ZhuLong) - a guest surface as a CPU-rendered target.
//
// The surface is staged in host memory and written back in one go, so guest
// writes go through GpuMemoryManager::GuestWrite exactly once per draw and code
// invalidation is routed for the whole range.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "zlong/gpu/memory/gpu_memory.h"
#include "zlong/gpu/render/draw.h"
#include "zlong/gpu/surface/format.h"
#include "zlong/gpu/surface/layout.h"
#include "zlong/gpu/surface/texel.h"

namespace zlong::gpu::software {

class SoftwareRenderTarget {
public:
    SoftwareRenderTarget(GpuMemoryManager& memory, const render::RenderTarget& target);

    bool valid() const noexcept { return valid_; }
    std::uint32_t width() const noexcept { return target_.width; }
    std::uint32_t height() const noexcept { return target_.height; }

    /// Fill every texel with the clear value.
    void Clear(const render::ClearState& state);

    bool Load(std::uint32_t x, std::uint32_t y, std::array<float, 4>& rgba) const;
    bool Store(std::uint32_t x, std::uint32_t y, const std::array<float, 4>& rgba);

    /// Publish the staged surface back to guest memory (routed, so executable
    /// and page-table ranges are reported).
    bool Flush();

private:
    GpuMemoryManager& memory_;
    render::RenderTarget target_{};
    surface::Surface surface_{};
    surface::FormatInfo info_{};
    std::vector<std::uint8_t> bytes_;
    bool valid_ = false;
};

}  // namespace zlong::gpu::software
