// 烛龙 (ZhuLong) - the 3D engine's register state machine.
//
// Turns decoded command-stream methods into a backend-neutral DrawDesc, which is
// the thing both render backends consume.
//
// The method addresses below are OUR model of the engine's register map: they
// are isolated here and in maxwell3d.cpp so filling in the real map later is a
// one-file change. Same treatment as the GOB layout and the method header.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zlong/gpu/engine/pushbuffer.h"
#include "zlong/gpu/memory/gpu_address_space.h"
#include "zlong/gpu/render/draw.h"

namespace zlong::gpu::engine {

/// Method addresses, in bytes of the engine's method space.
enum : std::uint32_t {
    /// [r, g, b, a] as float bits.
    kMethodClearColour = 0x0100,
    /// [enabled]
    kMethodClear = 0x0110,
    /// [pa_lo, pa_hi, width, height, pitch, format, tile]
    kMethodColourTarget = 0x0200,
    /// [x, y, width, height, min_depth, max_depth] as float bits.
    kMethodViewport = 0x0300,
    /// [pa_lo, pa_hi, size, stride]
    kMethodVertexBuffer = 0x0400,
    /// [location, buffer_index, offset, format, stride]
    kMethodVertexAttribute = 0x0500,
    /// [slot]; selects which shader slot the next draw uses.
    kMethodShader = 0x0600,
    /// [vertex_count]
    kMethodVertexCount = 0x0700,
    /// [] - triggers assembly of a draw description.
    kMethodDraw = 0x0800,
    /// [pa_lo, pa_hi, width, height, pitch, format, tile] - the depth target.
    kMethodDepthTarget = 0x0900,
    /// [test_enable, write_enable, compare]
    kMethodDepthState = 0x0A00,
    /// [cull_mode, front_face]
    kMethodCull = 0x0B00,
    /// [pa_lo, pa_hi, size, slot]. Replaces any binding already on that slot, so
    /// a stream can re-bind one buffer per object instead of accumulating.
    kMethodConstantBuffer = 0x0C00,
    /// [pa_lo, pa_hi, size, index_type]
    kMethodIndexBuffer = 0x0D00,
    /// [index_count]. Zero means the draw is not indexed.
    kMethodIndexCount = 0x0E00,
    /// [pa_lo, pa_hi, width, height, pitch, format, tile, slot,
    ///  min_filter, mag_filter, wrap_u, wrap_v]. Replaces by slot, like constants.
    kMethodTexture = 0x0F00,
};

class Maxwell3D {
public:
    Maxwell3D() = default;

    /// Shaders are bound by slot; the command stream selects a slot. The caller
    /// owns the modules and must keep them alive while they may be drawn with.
    void SetShader(std::size_t slot, const shader::Module* vertex,
                   const shader::Module* fragment);

    /// Start this engine with the same shader bindings as `other`.
    ///
    /// The shader table is the host's library standing in for the guest's own
    /// shader upload, which is not decoded yet. A channel the guest opens therefore
    /// begins with whatever the host installed, while everything a stream sets --
    /// the selected slot, the targets, the viewport -- stays that channel's.
    void CopyShadersFrom(const Maxwell3D& other);

    /// The address space every address a stream names is resolved through. Borrowed:
    /// there is one per device, shared by every channel and by nvmap, because that is
    /// what the hardware has.
    ///
    /// A null one resolves nothing, so every stream is refused -- deliberately. An
    /// engine that fell back to treating the value as a physical address would hide
    /// exactly the fault this exists to report: a stream naming memory nobody gave the
    /// GPU.
    void set_address_space(const GpuAddressSpace* space) noexcept { address_space_ = space; }

    /// Feed one decoded method. Returns false with `error` set only when a method
    /// whose arguments we need was malformed; unknown methods are counted and
    /// skipped so larger streams degrade instead of failing.
    bool Dispatch(const Method& method, const std::uint32_t* arguments, std::string& error);

    /// True when a draw has been assembled since the last TakeDraw().
    bool HasDraw() const noexcept { return pending_.has_value(); }
    /// Consume the pending draw description.
    std::optional<render::DrawDesc> TakeDraw();

    std::uint32_t skipped_methods() const noexcept { return skipped_methods_; }
    void Reset();

private:
    bool Expect(const Method& method, std::uint32_t minimum, std::string& error);

    /// Decode an address argument and resolve it through the address space.
    /// False with a reason when the address was never mapped.
    bool Address(const Method& method, const std::uint32_t* arguments, std::uint64_t& out,
                 std::string& error) const;

    struct ShaderSlot {
        const shader::Module* vertex = nullptr;
        const shader::Module* fragment = nullptr;
    };

    const GpuAddressSpace* address_space_ = nullptr;
    std::vector<ShaderSlot> shaders_;
    std::uint32_t selected_shader_slot_ = 0;
    std::uint32_t skipped_methods_ = 0;

    // Persistent pipeline state: it survives a draw, so a stream only re-sends
    // what changed.
    render::RenderTarget colour_target_{};
    bool has_colour_target_ = false;
    std::optional<render::RenderTarget> depth_target_;
    render::ClearState clear_{};
    render::Viewport viewport_{};
    render::DepthState depth_state_{};
    render::RasterState raster_{};

    // Per-draw bindings: geometry, constants and both counts are re-bound for
    // every draw, so assembling one clears them. A stream that meant to draw two
    // objects in a row has to say so twice -- which is what makes each draw
    // describe itself instead of depending on what happened to be bound before.
    std::vector<BufferBinding> vertex_buffers_;
    std::vector<render::VertexAttribute> attributes_;
    std::vector<ConstantBufferBinding> constants_;
    std::vector<TextureBinding> textures_;
    BufferBinding index_;
    render::IndexType index_type_ = render::IndexType::None;
    std::uint32_t index_count_ = 0;
    std::uint32_t vertex_count_ = 0;
    std::uint32_t vertex_offset_ = 0;

    std::optional<render::DrawDesc> pending_;
};

}  // namespace zlong::gpu::engine
