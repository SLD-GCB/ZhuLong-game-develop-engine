// 烛龙 (ZhuLong) - references to guest memory as a resource.
//
// Shared by the neutral draw description and the shader interpreter, so neither
// has to duplicate the other's vocabulary. No graphics API types here.

#pragma once

#include <cstdint>

#include "zlong/gpu/surface/format.h"
#include "zlong/gpu/surface/layout.h"
#include "zlong/gpu/types.h"

namespace zlong::gpu {

/// A window onto guest memory.
struct BufferBinding {
    GuestPa pa = 0;
    std::uint64_t size = 0;
    std::uint32_t stride = 0;
};

struct TextureRef {
    GuestPa pa = 0;
    surface::SurfaceFormat format = surface::SurfaceFormat::Unknown;
    surface::TileMode tile = surface::TileMode::Linear;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Bytes per row, linear only.
    std::uint32_t pitch = 0;
};

struct SamplerState {
    enum class Filter : std::uint8_t { Nearest, Linear };
    enum class Wrap : std::uint8_t { Repeat, Clamp, Mirror };

    Filter min_filter = Filter::Nearest;
    Filter mag_filter = Filter::Nearest;
    Wrap wrap_u = Wrap::Clamp;
    Wrap wrap_v = Wrap::Clamp;
};

struct TextureBinding {
    TextureRef texture;
    SamplerState sampler;
    std::uint32_t slot = 0;
};

struct ConstantBufferBinding {
    GuestPa pa = 0;
    std::uint64_t size = 0;
    std::uint32_t slot = 0;
};

/// Read-only view of guest memory. Lets the shader interpreter and the software
/// renderer read resources without depending on the Vulkan side or on
/// GpuMemoryManager's full interface.
class MemorySource {
public:
    virtual ~MemorySource() = default;
    /// Host pointer for a fully mapped range, or nullptr.
    virtual const std::uint8_t* peek(GuestPa pa, std::uint64_t size) const = 0;
};

}  // namespace zlong::gpu
