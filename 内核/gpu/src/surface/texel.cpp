#include "zlong/gpu/surface/texel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace zlong::gpu::surface {

namespace {

float Clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/// sRGB transfer function (the piecewise IEC 61966-2-1 curve).
float SrgbToLinear(float value) {
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float value) {
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

std::uint8_t ToUnorm8(float value) {
    const float scaled = Clamp01(value) * 255.0f + 0.5f;
    return static_cast<std::uint8_t>(scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled));
}

}  // namespace

std::array<float, 4> DecodeTexel(const FormatInfo& info, const std::uint8_t* bytes) {
    std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
    if (bytes == nullptr || info.bytes_per_pixel == 0) {
        return rgba;
    }

    switch (info.format) {
    case SurfaceFormat::R8G8B8A8_UNORM:
    case SurfaceFormat::R8G8B8A8_SRGB:
        rgba = {bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, bytes[3] / 255.0f};
        if (info.kind == ComponentKind::UnormSRGB) {
            rgba[0] = SrgbToLinear(rgba[0]);
            rgba[1] = SrgbToLinear(rgba[1]);
            rgba[2] = SrgbToLinear(rgba[2]);
        }
        return rgba;
    case SurfaceFormat::B8G8R8A8_UNORM:
    case SurfaceFormat::A8R8G8B8_UNORM:
        return {bytes[2] / 255.0f, bytes[1] / 255.0f, bytes[0] / 255.0f, bytes[3] / 255.0f};
    case SurfaceFormat::R5G6B5_UNORM: {
        const std::uint16_t packed = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
        return {static_cast<float>((packed >> 11) & 0x1F) / 31.0f,
                static_cast<float>((packed >> 5) & 0x3F) / 63.0f,
                static_cast<float>(packed & 0x1F) / 31.0f, 1.0f};
    }
    case SurfaceFormat::R4G4B4A4_UNORM: {
        // Little endian: byte 0 = G4B4, byte 1 = A4R4 (our model of the packing).
        const std::uint8_t low = bytes[0];
        const std::uint8_t high = bytes[1];
        return {static_cast<float>(high & 0x0F) / 15.0f, static_cast<float>(low >> 4) / 15.0f,
                static_cast<float>(low & 0x0F) / 15.0f, static_cast<float>(high >> 4) / 15.0f};
    }
    case SurfaceFormat::R8_UNORM:
        return {bytes[0] / 255.0f, bytes[0] / 255.0f, bytes[0] / 255.0f, 1.0f};
    case SurfaceFormat::R32G32B32A32_FLOAT: {
        std::array<float, 4> value{0, 0, 0, 1};
        std::memcpy(value.data(), bytes, 16);
        return value;
    }
    case SurfaceFormat::R32G32_FLOAT: {
        std::array<float, 4> value{0, 0, 0, 1};
        std::memcpy(value.data(), bytes, 8);
        return value;
    }
    case SurfaceFormat::D16_UNORM: {
        const std::uint16_t packed = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
        return {static_cast<float>(packed) / 65535.0f, 0.0f, 0.0f, 1.0f};
    }
    case SurfaceFormat::D24S8_UNORM: {
        const std::uint32_t packed = static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8) |
                                                               (bytes[2] << 16) | (bytes[3] << 24));
        return {static_cast<float>(packed & 0x00FFFFFFu) / 16777215.0f, 0.0f, 0.0f, 1.0f};
    }
    case SurfaceFormat::D32_FLOAT: {
        float value = 0.0f;
        std::memcpy(&value, bytes, sizeof(value));
        return {value, 0.0f, 0.0f, 1.0f};
    }
    case SurfaceFormat::Unknown:
        break;
    }
    return rgba;
}

void EncodeTexel(const FormatInfo& info, const std::array<float, 4>& rgba, std::uint8_t* bytes) {
    if (bytes == nullptr || info.bytes_per_pixel == 0) {
        return;
    }

    switch (info.format) {
    case SurfaceFormat::R8G8B8A8_UNORM:
    case SurfaceFormat::R8G8B8A8_SRGB: {
        float r = rgba[0];
        float g = rgba[1];
        float b = rgba[2];
        if (info.kind == ComponentKind::UnormSRGB) {
            r = LinearToSrgb(Clamp01(r));
            g = LinearToSrgb(Clamp01(g));
            b = LinearToSrgb(Clamp01(b));
        }
        bytes[0] = ToUnorm8(r);
        bytes[1] = ToUnorm8(g);
        bytes[2] = ToUnorm8(b);
        bytes[3] = ToUnorm8(rgba[3]);
        return;
    }
    case SurfaceFormat::B8G8R8A8_UNORM:
    case SurfaceFormat::A8R8G8B8_UNORM:
        bytes[0] = ToUnorm8(rgba[2]);
        bytes[1] = ToUnorm8(rgba[1]);
        bytes[2] = ToUnorm8(rgba[0]);
        bytes[3] = ToUnorm8(rgba[3]);
        return;
    case SurfaceFormat::R5G6B5_UNORM: {
        const auto r = static_cast<std::uint16_t>(Clamp01(rgba[0]) * 31.0f + 0.5f);
        const auto g = static_cast<std::uint16_t>(Clamp01(rgba[1]) * 63.0f + 0.5f);
        const auto b = static_cast<std::uint16_t>(Clamp01(rgba[2]) * 31.0f + 0.5f);
        const std::uint16_t packed =
            static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
        bytes[0] = static_cast<std::uint8_t>(packed & 0xFF);
        bytes[1] = static_cast<std::uint8_t>(packed >> 8);
        return;
    }
    case SurfaceFormat::R4G4B4A4_UNORM: {
        const auto r = static_cast<std::uint8_t>(Clamp01(rgba[0]) * 15.0f + 0.5f);
        const auto g = static_cast<std::uint8_t>(Clamp01(rgba[1]) * 15.0f + 0.5f);
        const auto b = static_cast<std::uint8_t>(Clamp01(rgba[2]) * 15.0f + 0.5f);
        const auto a = static_cast<std::uint8_t>(Clamp01(rgba[3]) * 15.0f + 0.5f);
        bytes[0] = static_cast<std::uint8_t>((g << 4) | b);
        bytes[1] = static_cast<std::uint8_t>((a << 4) | r);
        return;
    }
    case SurfaceFormat::R8_UNORM:
        bytes[0] = ToUnorm8(rgba[0]);
        return;
    case SurfaceFormat::R32G32B32A32_FLOAT:
        std::memcpy(bytes, rgba.data(), 16);
        return;
    case SurfaceFormat::R32G32_FLOAT:
        std::memcpy(bytes, rgba.data(), 8);
        return;
    case SurfaceFormat::D16_UNORM: {
        const auto packed =
            static_cast<std::uint16_t>(Clamp01(rgba[0]) * 65535.0f + 0.5f);
        bytes[0] = static_cast<std::uint8_t>(packed & 0xFF);
        bytes[1] = static_cast<std::uint8_t>(packed >> 8);
        return;
    }
    case SurfaceFormat::D24S8_UNORM: {
        const auto packed = static_cast<std::uint32_t>(Clamp01(rgba[0]) * 16777215.0f + 0.5f);
        bytes[0] = static_cast<std::uint8_t>(packed & 0xFF);
        bytes[1] = static_cast<std::uint8_t>((packed >> 8) & 0xFF);
        bytes[2] = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
        bytes[3] = 0;
        return;
    }
    case SurfaceFormat::D32_FLOAT: {
        const float value = Clamp01(rgba[0]);
        std::memcpy(bytes, &value, sizeof(value));
        return;
    }
    case SurfaceFormat::Unknown:
        break;
    }
}

}  // namespace zlong::gpu::surface
