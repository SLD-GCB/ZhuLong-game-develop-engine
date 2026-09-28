// 烛龙 (ZhuLong) - backend-neutral pipeline state enums.

#pragma once

#include <cstdint>

namespace zlong::gpu::render {

struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

enum class PrimitiveTopology : std::uint8_t { Points, Lines, Triangles, TriangleStrip };
enum class IndexType : std::uint8_t { None, UInt16, UInt32 };

enum class CompareOp : std::uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class CullMode : std::uint8_t { None, Front, Back };
enum class FrontFace : std::uint8_t { Clockwise, CounterClockwise };

enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
};

enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

struct RasterState {
    CullMode cull = CullMode::None;
    FrontFace front_face = FrontFace::CounterClockwise;
    bool scissor_enable = false;
};

struct DepthState {
    bool test_enable = false;
    bool write_enable = false;
    CompareOp compare = CompareOp::Less;
};

struct BlendState {
    bool enable = false;
    BlendFactor src_color = BlendFactor::One;
    BlendFactor dst_color = BlendFactor::Zero;
    BlendOp color_op = BlendOp::Add;
    BlendFactor src_alpha = BlendFactor::One;
    BlendFactor dst_alpha = BlendFactor::Zero;
    BlendOp alpha_op = BlendOp::Add;
    /// Per-channel write enables, bit 0 = R.
    std::uint8_t write_mask = 0x0F;
};

}  // namespace zlong::gpu::render
