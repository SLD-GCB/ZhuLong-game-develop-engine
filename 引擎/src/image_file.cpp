#include "zlong/engine/image_file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zlong::engine {

namespace {

/// 从 `bytes[at]` 读一个小端整数，**先看够不够长**。
///
/// 这一整份解析器都是在读别人的文件，所以每一次取字节都要问这一句 —— 少一处就是一个越界读，而
/// 越界读不是"报错"，是读一段别人的内存然后算出一个看着像模像样的尺寸。
bool At(const std::string& bytes, std::size_t at, std::size_t count) {
    return at + count <= bytes.size();
}

std::uint32_t Read32(const std::string& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

std::int32_t ReadSigned32(const std::string& bytes, std::size_t at) {
    return static_cast<std::int32_t>(Read32(bytes, at));
}

std::uint32_t Read16(const std::string& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8);
}

}  // namespace

bool ParseBmp(const std::string& bytes, Texture& out, std::string& error) {
    out = Texture{};

    if (!At(bytes, 0, 14) || bytes[0] != 'B' || bytes[1] != 'M') {
        error = "this does not start with \"BM\", so it is not a BMP";
        return false;
    }
    const std::uint32_t pixels_at = Read32(bytes, 10);
    const std::uint32_t dib_size = Read32(bytes, 14);

    if (dib_size < 40) {
        // 12 是 BITMAPCOREHEADER（OS/2 那个），比它更小的也不认。
        error = "this BMP's header says it is " + std::to_string(dib_size) +
                " bytes; I only read the 40-and-up ones (BITMAPINFOHEADER and later)";
        return false;
    }
    if (!At(bytes, 14, dib_size)) {
        error = "this file is too short for its own header (which says " +
                std::to_string(dib_size) + " bytes)";
        return false;
    }

    const std::int32_t width = ReadSigned32(bytes, 18);
    const std::int32_t height = ReadSigned32(bytes, 22);
    const std::uint32_t bits = Read16(bytes, 28);
    const std::uint32_t compression = Read32(bytes, 30);

    // **高度是负的表示"第一行在上"**，而它印出来和正的（第一行在下）不一样。两个都要认。
    // 用 64 位取反：`-height` 在 height 是最小那个数的时候会溢出。
    const std::int64_t rows =
        height < 0 ? -static_cast<std::int64_t>(height) : static_cast<std::int64_t>(height);
    const bool top_down = height < 0;
    const auto across = static_cast<std::uint32_t>(width);
    const auto down = static_cast<std::uint32_t>(rows);

    if (width <= 0 || rows <= 0 || rows > 0x7FFF'FFFF) {
        error = "this BMP is " + std::to_string(width) + " by " + std::to_string(height);
        return false;
    }
    if (bits != 24 && bits != 32) {
        error = "this is a " + std::to_string(bits) +
                "-bit BMP; I only read 24- and 32-bit uncompressed ones";
        return false;
    }
    if (compression != 0) {
        error = "this BMP is compressed (method " + std::to_string(compression) +
                "); I only read uncompressed ones";
        return false;
    }

    // 每行按 4 字节对齐。宽度是奇数的时候行尾有填充 —— 不跳过它，图会斜着扭曲。**乘法用 64 位**：
    // `宽 × 位数` 在宽是个大数的时候会绕回来，而绕回来的式子照样能通过"够不够长"那一问。
    const std::uint64_t stride = ((static_cast<std::uint64_t>(across) * bits + 31) / 32) * 4;
    const std::uint64_t needed =
        static_cast<std::uint64_t>(pixels_at) + stride * static_cast<std::uint64_t>(down);
    if (needed > bytes.size()) {
        error = "this BMP says it is " + std::to_string(across) + "x" + std::to_string(down) +
                " at " + std::to_string(bits) + " bits, which needs " + std::to_string(needed) +
                " bytes, and the file is " + std::to_string(bytes.size());
        return false;
    }

    out.width = across;
    out.height = down;
    out.rgba.resize(static_cast<std::size_t>(across) * down * 4);

    bool any_alpha = false;
    for (std::uint32_t row = 0; row < down; ++row) {
        // 文件里第一行在最后（除非高度是负的），而 `Texture` 要的是第一行在上。
        const std::uint32_t from = top_down ? row : down - 1 - row;
        const auto line = static_cast<std::size_t>(pixels_at) + static_cast<std::size_t>(from) *
                                                                     static_cast<std::size_t>(stride);
        for (std::uint32_t column = 0; column < across; ++column) {
            const std::size_t at = line + static_cast<std::size_t>(column) * (bits / 8);
            const std::size_t to = (static_cast<std::size_t>(row) * across + column) * 4;
            out.rgba[to + 0] = static_cast<std::uint8_t>(bytes[at + 2]);   // BGR 存的是蓝绿红
            out.rgba[to + 1] = static_cast<std::uint8_t>(bytes[at + 1]);
            out.rgba[to + 2] = static_cast<std::uint8_t>(bytes[at + 0]);
            const auto alpha = static_cast<std::uint8_t>(bits == 32 ? bytes[at + 3] : 255);
            out.rgba[to + 3] = alpha;
            if (alpha != 0) {
                any_alpha = true;
            }
        }
    }

    // 32 位那一档：第 4 个字节按标准是"保留"，可写图的工具一半拿它当 alpha、一半一律写 0。**全 0
    // 当作不透明** —— 否则每一张 32 位的图都是透明的，而那看起来像"图读坏了"。
    if (bits == 32 && !any_alpha) {
        for (std::size_t at = 3; at < out.rgba.size(); at += 4) {
            out.rgba[at] = 255;
        }
    }

    error.clear();
    return true;
}

bool LoadBmp(const std::string& path, Texture& out, std::string& error) {
    std::string contents;
    if (!ReadWholeFile(path, contents)) {
        error = "could not read " + path;
        return false;
    }
    return ParseBmp(contents, out, error);
}

}  // namespace zlong::engine
