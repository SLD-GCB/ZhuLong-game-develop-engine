// 烛龙 (ZhuLong) - the scene renderer: what turns a Scene into draws.
//
// The split with Scene is deliberate. A Scene is data with no idea what a GPU
// is; the renderer owns everything machine-side -- an arena in guest memory for
// the uploaded meshes, textures and per-drawable constants, the render targets,
// and the shader modules. Geometry is uploaded once in Prepare; only the
// per-drawable constants change between frames.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zlong/engine/scene.h"
#include "zlong/gpu/gpu.h"
#include "zlong/gpu/render/backend.h"
#include "zlong/gpu/shader/ir.h"

namespace zlong::engine {

class Renderer {
public:
    Renderer(zlong::gpu::Gpu& gpu, zlong::gpu::render::RenderBackend& backend);

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Upload the scene's meshes and textures, lay out the targets and the
    /// per-drawable constant slots, and install the shaders. Once per scene.
    ///
    /// The arena is the caller's block of guest memory -- the kernel hands one out, so
    /// `arena_base`/`arena_bytes` are what some other allocator reserved rather than
    /// numbers this class picked. A scene that does not fit is refused, which is what
    /// makes the reservation real: the renderer never walks past what it was given.
    bool Prepare(const Scene& scene, std::uint32_t width, std::uint32_t height,
                 zlong::gpu::GuestPa arena_base, std::uint64_t arena_bytes, std::string& error);

    /// Recompute each drawable node's world matrix and constants, build the
    /// command stream and submit it. `draws` receives how many draws went out.
    bool Render(const Scene& scene, std::size_t& draws, std::string& error);

    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    zlong::gpu::GuestPa colour_target() const noexcept { return colour_target_; }

    /// The colour target as the machine holds it: RGBA8, top row first.
    const std::uint8_t* colour_pixels() const;

    /// How many drawable nodes went out last time. Equal to the scene's drawable
    /// count until something starts culling.
    std::size_t submitted() const noexcept { return submitted_; }

    /// Bytes per row of the colour target.
    std::uint32_t target_pitch() const noexcept { return width_ * 4; }

    /// Where the last frame's time went, in milliseconds.
    ///
    /// Diagnostics, and deliberately part of the interface: "the generation is
    /// expensive" is a claim that has to be measured before anything is rebuilt to
    /// fix it, and a breakdown is the measurement.
    struct FrameProfile {
        /// World matrices, scene bounds and the frustum test.
        double prepare_ms = 0.0;
        /// Writing the per-drawable constants.
        double constants_ms = 0.0;
        /// Building the command words and writing them to guest memory.
        double stream_ms = 0.0;
        /// The GPU: decode, draws, and making the result visible.
        double submit_ms = 0.0;
    };
    const FrameProfile& profile() const noexcept { return profile_; }

private:
    struct UploadedMesh {
        zlong::gpu::GuestPa vertices = 0;
        zlong::gpu::GuestPa indices = 0;
        std::uint32_t vertex_bytes = 0;
        std::uint32_t index_bytes = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
    };

    zlong::gpu::GuestPa Allocate(std::size_t bytes, std::size_t alignment = 16);
    /// The GPU address of a range in the arena. A stream carries GPU addresses -- the
    /// decoder resolves every one of them -- so this is the only way an address reaches
    /// a command word.
    zlong::gpu::GpuVAddr OnGpu(zlong::gpu::GuestPa pa) const;
    bool Write(zlong::gpu::GuestPa at, const void* data, std::size_t bytes);

    zlong::gpu::Gpu& gpu_;
    zlong::gpu::render::RenderBackend& backend_;

    std::vector<UploadedMesh> meshes_;
    /// Local-space bounds of each uploaded mesh, for the per-frame cull test.
    std::vector<Bound> mesh_bounds_;
    std::vector<zlong::gpu::GuestPa> textures_;
    /// One 192-byte block per drawable node, so a frame changes constants and
    /// nothing else. Slot `d` belongs to node `drawable_nodes_[d]`.
    std::vector<zlong::gpu::GuestPa> constant_slots_;
    /// One block per drawable for the shadow pass, which needs the light-space
    /// matrix where the main pass needs the camera's.
    std::vector<zlong::gpu::GuestPa> shadow_slots_;
    /// The index of every drawable node, in scene order.
    std::vector<std::uint32_t> drawable_nodes_;
    /// The scene's node count at Prepare time, so a re-shaped scene is refused
    /// instead of drawn against stale slots.
    std::size_t node_count_ = 0;

    // Members rather than a vector: the gpu holds POINTERS to these, and a
    // vector would move them out from under it.
    zlong::gpu::shader::Module vertex_shader_;
    /// Indexed by variant: 0 untextured, 1 textured, 2 normal-mapped, and 3..5
    /// the same three with the shadow term.
    std::array<zlong::gpu::shader::Module, 6> fragment_shaders_;
    /// The shadow pass writes depth and nothing else.
    zlong::gpu::shader::Module shadow_fragment_;
    /// Non-empty when assembling the shadow fragment shader failed. Its words are
    /// written in renderer.cpp and decoded here, so a failure means the engine and
    /// the Maxwell encoder have drifted -- there is no shader to fall back to, and
    /// Prepare and Render refuse rather than draw with one that does nothing.
    std::string shader_error_;

    FrameProfile profile_{};
    bool shadows_ = false;
    std::uint32_t shadow_size_ = 0;
    zlong::gpu::GuestPa shadow_colour_ = 0;
    zlong::gpu::GuestPa shadow_depth_ = 0;
    /// One transform per frame, built from the first light and the scene bounds.
    Mat4 shadow_matrix_{};

    zlong::gpu::GuestPa colour_target_ = 0;
    zlong::gpu::GuestPa depth_target_ = 0;
    zlong::gpu::GuestPa stream_ = 0;
    std::size_t stream_capacity_ = 0;
    /// The block the caller reserved, and how far into it this renderer has walked.
    /// `Allocate` refuses to leave it, so the reservation is a boundary and not a hint.
    zlong::gpu::GuestPa arena_base_ = 0;
    std::uint64_t arena_bytes_ = 0;
    zlong::gpu::GuestPa arena_cursor_ = 0;
    bool arena_overflow_ = false;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t submitted_ = 0;
};

}  // namespace zlong::engine
