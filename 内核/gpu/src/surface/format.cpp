#include "zlong/gpu/surface/format.h"

namespace zlong::gpu::surface {

namespace {

constexpr FormatInfo Make(SurfaceFormat format, std::uint32_t bpp, std::uint8_t components,
                          ComponentKind kind, bool is_depth = false,
                          FormatConversion conversion = FormatConversion::None) {
    return FormatInfo{
        format, bpp, components, kind, is_depth,
        conversion != FormatConversion::None, conversion,
    };
}

}  // namespace

FormatInfo DecodeFormat(SurfaceFormat format) noexcept {
    switch (format) {
    case SurfaceFormat::R8G8B8A8_UNORM:
        return Make(format, 4, 4, ComponentKind::Unorm);
    case SurfaceFormat::R8G8B8A8_SRGB:
        // sRGB is carried by the component kind; the write/read path encodes or
        // decodes, so no separate conversion step is needed.
        return Make(format, 4, 4, ComponentKind::UnormSRGB);
    case SurfaceFormat::B8G8R8A8_UNORM:
        return Make(format, 4, 4, ComponentKind::Unorm, false, FormatConversion::SwapRB);
    case SurfaceFormat::A8R8G8B8_UNORM:
        // Little-endian byte order is B,G,R,A, i.e. the same swap as BGRA.
        return Make(format, 4, 4, ComponentKind::Unorm, false, FormatConversion::SwapRB);
    case SurfaceFormat::R5G6B5_UNORM:
        return Make(format, 2, 3, ComponentKind::Unorm, false, FormatConversion::Unpack565);
    case SurfaceFormat::R4G4B4A4_UNORM:
        return Make(format, 2, 4, ComponentKind::Unorm, false, FormatConversion::Unpack4444);
    case SurfaceFormat::R8_UNORM:
        return Make(format, 1, 1, ComponentKind::Unorm);
    case SurfaceFormat::R32G32B32A32_FLOAT:
        return Make(format, 16, 4, ComponentKind::Float);
    case SurfaceFormat::R32G32_FLOAT:
        return Make(format, 8, 2, ComponentKind::Float);
    case SurfaceFormat::D16_UNORM:
        return Make(format, 2, 1, ComponentKind::Depth, /*is_depth=*/true);
    case SurfaceFormat::D24S8_UNORM:
        return Make(format, 4, 2, ComponentKind::DepthStencil, /*is_depth=*/true);
    case SurfaceFormat::D32_FLOAT:
        return Make(format, 4, 1, ComponentKind::Float, /*is_depth=*/true);
    case SurfaceFormat::Unknown:
        break;
    }
    return FormatInfo{};
}

bool IsColourTarget(SurfaceFormat format) noexcept {
    const FormatInfo info = DecodeFormat(format);
    return format != SurfaceFormat::Unknown && !info.is_depth;
}

bool IsDepthTarget(SurfaceFormat format) noexcept {
    return DecodeFormat(format).is_depth;
}

}  // namespace zlong::gpu::surface
