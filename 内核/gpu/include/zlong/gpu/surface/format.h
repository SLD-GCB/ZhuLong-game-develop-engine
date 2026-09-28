// 烛龙 (ZhuLong) - guest surface formats, described neutrally.
//
// Vulkan is deliberately absent here: this table is shared by the Vulkan
// backend and the CPU (software) backend, and the CPU-only build must not need
// the Vulkan SDK.

#pragma once

#include <cstdint>

namespace zlong::gpu::surface {

enum class SurfaceFormat : std::uint16_t {
    Unknown = 0,
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    B8G8R8A8_UNORM,
    A8R8G8B8_UNORM,
    R5G6B5_UNORM,
    R4G4B4A4_UNORM,
    R8_UNORM,
    /// Vertex attributes and float render targets.
    R32G32B32A32_FLOAT,
    R32G32_FLOAT,
    D16_UNORM,
    D24S8_UNORM,
    D32_FLOAT,
};

enum class ComponentKind : std::uint8_t {
    Unorm,
    UnormSRGB,
    Float,
    Uint,
    Sint,
    Depth,
    DepthStencil,
};

/// What a consumer must do beyond reading the raw bytes.
enum class FormatConversion : std::uint8_t {
    None,
    /// Byte order is swapped relative to R8G8B8A8 (e.g. B8G8R8A8, A8R8G8B8).
    SwapRB,
    /// 16-bit packed 5:6:5.
    Unpack565,
    /// 16-bit packed 4:4:4:4.
    Unpack4444,
};

struct FormatInfo {
    SurfaceFormat format = SurfaceFormat::Unknown;
    std::uint32_t bytes_per_pixel = 0;
    std::uint8_t components = 0;
    ComponentKind kind = ComponentKind::Unorm;
    bool is_depth = false;
    bool needs_conversion = false;
    FormatConversion conversion = FormatConversion::None;
};

FormatInfo DecodeFormat(SurfaceFormat format) noexcept;

/// Usable as a colour render target.
bool IsColourTarget(SurfaceFormat format) noexcept;
/// Usable as a depth/stencil target.
bool IsDepthTarget(SurfaceFormat format) noexcept;

}  // namespace zlong::gpu::surface
