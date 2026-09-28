// 烛龙 (ZhuLong) - per-texel format conversion.
//
// One place for turning a format's raw bytes into linear floating RGBA and back,
// shared by the shader interpreter (texture reads) and the software renderer
// (render-target reads/writes).

#pragma once

#include <array>
#include <cstdint>

#include "zlong/gpu/surface/format.h"

namespace zlong::gpu::surface {

/// Decode one texel. `bytes` must point at `DecodeFormat(format).bytes_per_pixel`
/// bytes. Components are 0..1 for UNORM/UNORM_SRGB formats.
std::array<float, 4> DecodeTexel(const FormatInfo& info, const std::uint8_t* bytes);

/// Encode linear floating RGBA into one texel.
void EncodeTexel(const FormatInfo& info, const std::array<float, 4>& rgba, std::uint8_t* bytes);

}  // namespace zlong::gpu::surface
