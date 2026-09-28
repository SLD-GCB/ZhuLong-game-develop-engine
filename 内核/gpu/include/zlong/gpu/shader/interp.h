// 烛龙 (ZhuLong) - the shader IR interpreter.
//
// This is the CPU render path's shader engine: the same IR the SPIR-V emitter
// consumes is executed here directly, so a machine with no GPU runs real shaders
// without needing a compiler.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "zlong/gpu/resource.h"
#include "zlong/gpu/shader/ir.h"

namespace zlong::gpu::shader {

/// Everything one shader invocation reads, and where it writes its results.
struct ShaderIo {
    /// VS: one entry per Module::inputs slot (vertex attributes).
    /// FS: one entry per Module::inputs slot (interpolated varyings).
    std::vector<std::array<float, 4>> inputs;

    /// ReadSpecial targets, indexed by the Special enum:
    /// VertexIndex, InstanceIndex, FragCoord, FrontFacing.
    std::array<std::array<float, 4>, 4> special{};

    /// VS: outputs[0] is clip-space position, outputs[1..] are varyings.
    /// FS: outputs[0] is colour attachment 0.
    std::vector<std::array<float, 4>> outputs;

    const std::vector<ConstantBufferBinding>* constants = nullptr;
    const std::vector<TextureBinding>* textures = nullptr;
    const MemorySource* memory = nullptr;
};

struct InterpResult {
    bool killed = false;
    /// Empty on success; otherwise why execution stopped.
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

/// Execute one invocation. Never throws.
InterpResult Interpret(const Module& module, const ShaderIo& in, ShaderIo& out);

}  // namespace zlong::gpu::shader
