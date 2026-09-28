// 烛龙 (ZhuLong) - the CPU rasterizer: vertex shading, triangle setup, the
// fragment stage (shader + depth + blend), and writeback.
//
// Deliberately simple and correct rather than fast: this is the path for
// machines with no usable GPU, and the always-available reference for checking
// the GPU path against.

#pragma once

#include "zlong/gpu/memory/gpu_memory.h"
#include "zlong/gpu/render/backend.h"
#include "zlong/gpu/render/draw.h"
#include "zlong/gpu/software/pool.h"

namespace zlong::gpu::software {

class Rasterizer {
public:
    Rasterizer(const render::DrawDesc& draw, GpuMemoryManager& memory, ThreadPool& pool);

    /// Runs to completion and leaves the colour target in guest memory.
    render::RenderResult Draw();

private:
    const render::DrawDesc& draw_;
    GpuMemoryManager& memory_;
    ThreadPool& pool_;
};

}  // namespace zlong::gpu::software
