#include "glyph.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#pragma warning(push, 0)
#include <windows.h>
#pragma warning(pop)
#endif

namespace zlong::xiangqi {

namespace {

/// How much bigger than the final texture the glyph is drawn before being boxed down.
constexpr int kSupersample = 4;

constexpr std::uint32_t kFace = 0xE2D2AC;  // boxwood

std::uint8_t Red(std::uint32_t colour) { return static_cast<std::uint8_t>((colour >> 16) & 0xFFu); }
std::uint8_t Green(std::uint32_t colour) { return static_cast<std::uint8_t>((colour >> 8) & 0xFFu); }
std::uint8_t Blue(std::uint32_t colour) { return static_cast<std::uint8_t>(colour & 0xFFu); }

/// A face with a ring and no character, which is what a platform without a text API gets.
/// The board still reads -- every piece is a disc -- the kinds simply do not.
engine::Texture PlainFace(std::uint32_t ring, std::uint32_t size) {
    engine::Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.assign(static_cast<std::size_t>(size) * size * 4, 255);
    const float centre = static_cast<float>(size) * 0.5f;
    const float outer = centre - static_cast<float>(size) / 12.0f;
    const float inner = outer - static_cast<float>(size) / 20.0f;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - centre;
            const float dy = static_cast<float>(y) + 0.5f - centre;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const std::uint32_t colour = (distance <= outer && distance >= inner) ? ring : kFace;
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            texture.rgba[at + 0] = Red(colour);
            texture.rgba[at + 1] = Green(colour);
            texture.rgba[at + 2] = Blue(colour);
        }
    }
    return texture;
}

}  // namespace

engine::Texture MakePieceFace(const wchar_t* glyph, std::uint32_t ink, std::uint32_t ring,
                              std::uint32_t size) {
#if defined(_WIN32)
    const int big = static_cast<int>(size) * kSupersample;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = big;
    info.bmiHeader.biHeight = -big;  // top row first, like the engine's textures
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dc == nullptr || bitmap == nullptr || bits == nullptr) {
        if (dc != nullptr) {
            DeleteDC(dc);
        }
        return PlainFace(ring, size);
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);

    const auto colour = [](std::uint32_t hex) {
        return RGB(Red(hex), Green(hex), Blue(hex));
    };
    RECT full{0, 0, big, big};
    HBRUSH brush = CreateSolidBrush(colour(kFace));
    FillRect(dc, &full, brush);
    DeleteObject(brush);

    // The ring, which is the side's colour: a piece is boxwood either way, so this is half
    // of what tells red from black.
    HPEN pen = CreatePen(PS_SOLID, std::max(1, big / 40), colour(ring));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int inset = big / 12;
    Ellipse(dc, inset, inset, big - inset, big - inset);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);

    // The character, upright: the half turn the camera needs is applied by the piece's own
    // node, because DrawText with a rotated font lays the text out of the rect. KaiTi if
    // this machine has it, and Windows substitutes a serif if not.
    HFONT font = CreateFontW(-static_cast<int>(static_cast<double>(big) * 0.68), 0, 0, 0,
                             FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"KaiTi");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour(ink));
    DrawTextW(dc, glyph, -1, &full, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(dc, old_font);
    DeleteObject(font);
    GdiFlush();

    // Box it down. The DIB is BGRA and the texture is RGBA.
    engine::Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.assign(static_cast<std::size_t>(size) * size * 4, 255);
    const auto* source = static_cast<const std::uint8_t*>(bits);
    const int samples = kSupersample * kSupersample;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            int sum[3] = {0, 0, 0};
            for (int dy = 0; dy < kSupersample; ++dy) {
                for (int dx = 0; dx < kSupersample; ++dx) {
                    const std::size_t at =
                        (static_cast<std::size_t>(y * kSupersample + dy) * big +
                         (x * kSupersample + dx)) *
                        4;
                    sum[0] += source[at + 2];
                    sum[1] += source[at + 1];
                    sum[2] += source[at + 0];
                }
            }
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            texture.rgba[at + 0] = static_cast<std::uint8_t>(sum[0] / samples);
            texture.rgba[at + 1] = static_cast<std::uint8_t>(sum[1] / samples);
            texture.rgba[at + 2] = static_cast<std::uint8_t>(sum[2] / samples);
        }
    }

    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return texture;
#else
    (void)glyph;
    (void)ink;
    return PlainFace(ring, size);
#endif
}

}  // namespace zlong::xiangqi
