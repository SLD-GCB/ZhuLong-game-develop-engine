// 烛龙 (ZhuLong) - IR to SPIR-V.
//
// The second consumer of the shader IR (the first is shader/interp). Numbers are
// emitted as function-local float variables, one per (register, lane), which
// keeps everything straight-line and avoids SSA construction.
//
// Scope today: load-attribute, load-immediate, move, read-special, store-output,
// kill, return, and the float arithmetic set. Predicates, control flow, integer
// and texture operations are reported as errors rather than mis-emitted.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "zlong/gpu/shader/ir.h"

namespace zlong::gpu::shader {

std::optional<std::vector<std::uint32_t>> EmitSpirv(const Module& module, std::string& error);

}  // namespace zlong::gpu::shader
