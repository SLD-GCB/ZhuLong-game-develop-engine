// 烛龙 (ZhuLong) - guest surface memory layouts.
//
// The block-linear (GOB) addressing formula lives ONLY in layout.cpp. Treat it
// as our model of the layout rather than verified hardware behaviour: the real
// Tegra layout interleaves GOBs in groups, and correcting that is a change to
// one file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zlong::gpu::surface {

enum class TileMode : std::uint8_t {
    Linear = 0,
    /// 16-byte-wide, 16-row GOBs (256 bytes each).
    BlockLinear16Bx16,
};

struct Surface {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Bytes per row, linear only. Must be >= width * bpp; 0 means "tight".
    /// Padding bytes implied by a larger pitch are never touched.
    std::uint32_t pitch = 0;
    TileMode tile = TileMode::Linear;
    std::uint32_t bpp = 4;
};

/// Byte offset of texel (x, y), or `kInvalidOffset` when out of range.
inline constexpr std::size_t kInvalidOffset = static_cast<std::size_t>(-1);

std::size_t OffsetOf(const Surface& surface, std::uint32_t x, std::uint32_t y);

/// Bytes needed to hold the whole surface.
std::size_t SurfaceSize(const Surface& surface);

/// Convert a tightly packed linear image (width * bpp per row) into the
/// surface's layout.
void SwizzleIn(std::span<const std::uint8_t> linear, const Surface& surface,
               std::span<std::uint8_t> out);

/// Convert the surface's layout into a tightly packed linear image.
void SwizzleOut(std::span<const std::uint8_t> stored, const Surface& surface,
                std::span<std::uint8_t> out);

}  // namespace zlong::gpu::surface
