// 烛龙 (ZhuLong) - write a rendered frame out as a BMP.
//
// 24-bit uncompressed, which is a few dozen lines and no dependency: the point
// is to be able to LOOK at a frame, not to ship an image library.

#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace host {

/// `rgba` is the colour target as it sits in guest memory: RGBA8, top row first,
/// `pitch` bytes per row. BMP wants BGR, bottom row first, rows padded to four
/// bytes -- so this is a real conversion, not a memcpy.
inline bool WriteBmp(const std::string& path, const std::uint8_t* rgba, std::uint32_t width,
                     std::uint32_t height, std::uint32_t pitch) {
    if (rgba == nullptr || width == 0 || height == 0) {
        return false;
    }

    const std::uint32_t row_bytes = width * 3;
    const std::uint32_t padding = (4 - (row_bytes % 4)) % 4;
    const std::uint32_t stride = row_bytes + padding;
    const std::uint32_t pixel_bytes = stride * height;

    const std::uint32_t file_header = 14;
    const std::uint32_t info_header = 40;
    const std::uint32_t offset = file_header + info_header;
    const std::uint32_t total = offset + pixel_bytes;

    std::vector<std::uint8_t> out(total, 0);
    const auto put16 = [&](std::uint32_t at, std::uint16_t value) {
        out[at] = static_cast<std::uint8_t>(value & 0xFFu);
        out[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    };
    const auto put32 = [&](std::uint32_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out[at + static_cast<std::uint32_t>(i)] =
                static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
    };

    out[0] = 'B';
    out[1] = 'M';
    put32(2, total);
    put32(10, offset);
    put32(14, info_header);
    put32(18, width);
    put32(22, height);
    put16(26, 1);   // planes
    put16(28, 24);  // bits per pixel
    put32(34, pixel_bytes);

    for (std::uint32_t row = 0; row < height; ++row) {
        // Bottom-up: the file's first row is the image's last.
        const std::uint8_t* source = rgba + static_cast<std::size_t>(height - 1 - row) * pitch;
        std::uint8_t* destination = out.data() + offset + static_cast<std::size_t>(row) * stride;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint8_t* pixel = source + static_cast<std::size_t>(column) * 4;
            destination[column * 3 + 0] = pixel[2];  // blue
            destination[column * 3 + 1] = pixel[1];  // green
            destination[column * 3 + 2] = pixel[0];  // red
        }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return file.good();
}

}  // namespace host
