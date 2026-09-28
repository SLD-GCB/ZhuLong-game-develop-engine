#include "zlong/gpu/surface/layout.h"

#include <cstring>

namespace zlong::gpu::surface {

namespace {

constexpr std::uint32_t kGobWidth = 16;   // bytes
constexpr std::uint32_t kGobRows = 16;    // rows
constexpr std::uint32_t kGobBytes = kGobWidth * kGobRows;

std::uint32_t EffectiveBpp(const Surface& surface) {
    return surface.bpp == 0 ? 1 : surface.bpp;
}

std::uint32_t EffectivePitch(const Surface& surface) {
    const std::uint32_t tight = surface.width * EffectiveBpp(surface);
    return surface.pitch < tight ? tight : surface.pitch;
}

std::uint32_t GobsPerRow(const Surface& surface) {
    const std::uint32_t bytes_per_row = surface.width * EffectiveBpp(surface);
    return (bytes_per_row + kGobWidth - 1) / kGobWidth;
}

}  // namespace

std::size_t OffsetOf(const Surface& surface, std::uint32_t x, std::uint32_t y) {
    if (x >= surface.width || y >= surface.height) {
        return kInvalidOffset;
    }
    const std::uint32_t bpp = EffectiveBpp(surface);

    if (surface.tile == TileMode::Linear) {
        return static_cast<std::size_t>(y) * EffectivePitch(surface) +
               static_cast<std::size_t>(x) * bpp;
    }

    // Block-linear: a GOB is 16 bytes wide and 16 rows tall. GOBs are laid out
    // row-major across the surface. (Model, not hardware-verified.)
    const std::uint64_t byte_x = static_cast<std::uint64_t>(x) * bpp;
    const std::uint64_t gob_x = byte_x / kGobWidth;
    const std::uint64_t in_gob_x = byte_x % kGobWidth;
    const std::uint64_t gob_y = y / kGobRows;
    const std::uint64_t in_gob_y = y % kGobRows;
    const std::uint64_t gob_index = gob_y * GobsPerRow(surface) + gob_x;
    return static_cast<std::size_t>(gob_index * kGobBytes + in_gob_y * kGobWidth + in_gob_x);
}

std::size_t SurfaceSize(const Surface& surface) {
    if (surface.tile == TileMode::Linear) {
        return static_cast<std::size_t>(EffectivePitch(surface)) * surface.height;
    }
    const std::uint32_t gob_rows = (surface.height + kGobRows - 1) / kGobRows;
    return static_cast<std::size_t>(GobsPerRow(surface)) * gob_rows * kGobBytes;
}

void SwizzleIn(std::span<const std::uint8_t> linear, const Surface& surface,
               std::span<std::uint8_t> out) {
    const std::uint32_t bpp = EffectiveBpp(surface);
    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * bpp;
    if (linear.size() < row_bytes * surface.height || out.size() < SurfaceSize(surface)) {
        return;
    }
    for (std::uint32_t y = 0; y < surface.height; ++y) {
        for (std::uint32_t x = 0; x < surface.width; ++x) {
            const std::size_t src = static_cast<std::size_t>(y) * row_bytes +
                                    static_cast<std::size_t>(x) * bpp;
            const std::size_t dst = OffsetOf(surface, x, y);
            std::memcpy(out.data() + dst, linear.data() + src, bpp);
        }
    }
}

void SwizzleOut(std::span<const std::uint8_t> stored, const Surface& surface,
                std::span<std::uint8_t> out) {
    const std::uint32_t bpp = EffectiveBpp(surface);
    const std::size_t row_bytes = static_cast<std::size_t>(surface.width) * bpp;
    if (out.size() < row_bytes * surface.height || stored.size() < SurfaceSize(surface)) {
        return;
    }
    for (std::uint32_t y = 0; y < surface.height; ++y) {
        for (std::uint32_t x = 0; x < surface.width; ++x) {
            const std::size_t src = OffsetOf(surface, x, y);
            const std::size_t dst = static_cast<std::size_t>(y) * row_bytes +
                                    static_cast<std::size_t>(x) * bpp;
            std::memcpy(out.data() + dst, stored.data() + src, bpp);
        }
    }
}

}  // namespace zlong::gpu::surface
