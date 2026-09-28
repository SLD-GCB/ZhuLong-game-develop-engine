#include "zlong/gpu/software/target.h"

namespace zlong::gpu::software {

SoftwareRenderTarget::SoftwareRenderTarget(GpuMemoryManager& memory,
                                           const render::RenderTarget& target)
    : memory_(memory), target_(target) {
    info_ = surface::DecodeFormat(target.format);
    if (info_.bytes_per_pixel == 0 || target.width == 0 || target.height == 0) {
        return;
    }

    surface_.width = target.width;
    surface_.height = target.height;
    surface_.pitch = target.pitch;
    surface_.tile = target.tile;
    surface_.bpp = info_.bytes_per_pixel;

    const std::size_t size = surface::SurfaceSize(surface_);
    if (size == 0) {
        return;
    }
    bytes_.assign(size, 0);

    // Stage the current contents so a partial draw preserves untouched texels.
    // A failure here means the range is not mapped: that is a bad target.
    if (!memory_.GuestRead(target.pa, bytes_.data(), size)) {
        return;
    }
    valid_ = true;
}

void SoftwareRenderTarget::Clear(const render::ClearState& state) {
    if (!valid_) {
        return;
    }
    const std::array<float, 4> colour =
        info_.is_depth ? std::array<float, 4>{state.depth, 0.0f, 0.0f, 1.0f} : state.color;
    std::array<std::uint8_t, 16> encoded{};
    surface::EncodeTexel(info_, colour, encoded.data());
    for (std::uint32_t y = 0; y < target_.height; ++y) {
        for (std::uint32_t x = 0; x < target_.width; ++x) {
            const std::size_t offset = surface::OffsetOf(surface_, x, y);
            if (offset == surface::kInvalidOffset) {
                continue;
            }
            for (std::uint32_t byte = 0; byte < info_.bytes_per_pixel; ++byte) {
                bytes_[offset + byte] = encoded[byte];
            }
        }
    }
}

bool SoftwareRenderTarget::Load(std::uint32_t x, std::uint32_t y,
                                std::array<float, 4>& rgba) const {
    if (!valid_) {
        return false;
    }
    const std::size_t offset = surface::OffsetOf(surface_, x, y);
    if (offset == surface::kInvalidOffset) {
        return false;
    }
    rgba = surface::DecodeTexel(info_, bytes_.data() + offset);
    return true;
}

bool SoftwareRenderTarget::Store(std::uint32_t x, std::uint32_t y,
                                 const std::array<float, 4>& rgba) {
    if (!valid_) {
        return false;
    }
    const std::size_t offset = surface::OffsetOf(surface_, x, y);
    if (offset == surface::kInvalidOffset) {
        return false;
    }
    surface::EncodeTexel(info_, rgba, bytes_.data() + offset);
    return true;
}

bool SoftwareRenderTarget::Flush() {
    if (!valid_ || bytes_.empty()) {
        return false;
    }
    return memory_.GuestWrite(target_.pa, bytes_.data(), bytes_.size());
}

}  // namespace zlong::gpu::software
