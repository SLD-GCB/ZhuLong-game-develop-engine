// 烛龙 (ZhuLong) - the backend-neutral draw description.
//
// This is THE contract between the command/state layer and every render backend.
// It contains only guest physical addresses, our own enums, surface:: types and
// shader::Module pointers -- no graphics API types, no handles, no ownership. A
// backend resolves the addresses itself (the Vulkan backend is the only place a
// binding becomes a VkBuffer).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "zlong/gpu/render/state.h"
#include "zlong/gpu/resource.h"
#include "zlong/gpu/shader/ir.h"
#include "zlong/gpu/surface/format.h"
#include "zlong/gpu/surface/layout.h"
#include "zlong/gpu/types.h"

namespace zlong::gpu::render {

struct VertexAttribute {
    /// Matches the shader module's input Location.
    std::uint32_t location = 0;
    /// Index into DrawDesc::vertex_buffers.
    std::uint32_t buffer_index = 0;
    /// Byte offset of the attribute's first element inside the buffer.
    std::uint32_t offset = 0;
    surface::SurfaceFormat format = surface::SurfaceFormat::Unknown;
    std::uint32_t stride = 0;
};

struct RenderTarget {
    GuestPa pa = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Bytes per row, linear only. Ignored when tiled.
    std::uint32_t pitch = 0;
    surface::SurfaceFormat format = surface::SurfaceFormat::Unknown;
    surface::TileMode tile = surface::TileMode::Linear;
};

struct ClearState {
    bool enabled = false;
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.0f};
    float depth = 1.0f;
    std::uint8_t stencil = 0;
};

struct DrawDesc {
    RenderTarget color;
    std::optional<RenderTarget> depth;

    Viewport viewport;
    Rect scissor;
    RasterState raster;
    DepthState depth_state;
    BlendState blend;
    ClearState clear;

    // The structural pivot of this layer: both backends consume the same IR.
    const shader::Module* vertex_shader = nullptr;
    const shader::Module* fragment_shader = nullptr;

    std::vector<VertexAttribute> attributes;
    std::vector<BufferBinding> vertex_buffers;

    BufferBinding index;
    IndexType index_type = IndexType::None;

    std::vector<ConstantBufferBinding> constants;
    std::vector<TextureBinding> textures;

    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    std::uint32_t vertex_count = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t index_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
};

}  // namespace zlong::gpu::render
