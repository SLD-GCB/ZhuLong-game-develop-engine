// 烛龙 (ZhuLong) - shader intermediate representation.
//
// A register machine over a control-flow graph, not SSA: registers and
// predicates map straight onto SPIR-V function-local variables, so no SSA
// construction and no Phi are needed.
//
// Two consumers share this IR:
//   * shader/interp   - interprets it (the CPU/software render path)
//   * shader/spirv_emitter - translates it (the Vulkan path)

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace zlong::gpu::shader {

enum class Stage : std::uint8_t { Vertex, Fragment };
enum class RegKind : std::uint8_t { Gpr, Predicate };
enum class ScalarType : std::uint8_t { F32, U32, S32 };

/// Comparison selector for the set-predicate ops.
enum class Compare : std::uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class Op : std::uint16_t {
    Nop,
    Label,
    Branch,
    BranchConditional,
    Return,
    Kill,

    Mov,
    LoadImm,
    ReadSpecial,
    LoadAttribute,

    SetP,
    FSetP,
    ISetP,

    FAdd,
    FSub,
    FMul,
    FFma,
    FMin,
    FMax,
    FNeg,
    FAbs,
    FFloor,
    FRcp,
    FSqrt,
    /// dst[component] = sum over lanes of a * b.
    ///
    /// Writes ONE lane, not all four, because a reduction produces a scalar and
    /// this IR puts scalars in lanes -- the same way LoadImm does. Without it
    /// there is no horizontal add anywhere, so no dot product and no matrix
    /// multiply: a vertex shader could not project anything.
    FDot,
    /// All four lanes of dst = src[component].
    ///
    /// The other half of FDot. A reduction produces a scalar, and to use that
    /// scalar on a vector it has to be spread back over the lanes -- without
    /// this, dot(normal, light) can be computed and then not used.
    Splat,
    F2F,
    F2I,
    I2F,

    IAdd,
    ISub,
    IMul,
    IAnd,
    IOr,
    IXor,
    IShl,
    IShr,
    Lop3,

    LoadConst,
    LoadLocal,
    StoreLocal,
    LoadGlobal,
    StoreGlobal,
    LoadTexture,
    SampleTexture,

    Interpolate,
    StoreOutput,
};

enum class Special : std::uint8_t {
    VertexIndex,
    InstanceIndex,
    FragCoord,
    FrontFacing,
    PointCoord,
};

/// A register, a lane of one, or a predicate.
///
/// A GPR index at or past `Function::gpr_count` is the zero register: it reads zero
/// and a write to it goes nowhere. A shader's own encoding names one -- Maxwell's RZ
/// is register 255 -- and both consumers read it the same way, so a decoded
/// instruction can name it without either consumer having to know how large the
/// other's register file turned out to be.
struct Value {
    RegKind kind = RegKind::Gpr;
    std::uint8_t index = 0;
    std::uint8_t component = 0;
};

struct Instr {
    Op op = Op::Nop;
    ScalarType type = ScalarType::F32;

    std::array<Value, 3> src{};
    std::uint8_t src_count = 0;
    Value dst{};

    bool predicated = false;
    Value predicate{};
    bool pred_negate = false;

    std::array<std::uint8_t, 4> swizzle{0, 1, 2, 3};
    bool abs = false;
    bool negate = false;

    float imm_f = 0.0f;
    std::uint32_t imm_u = 0;

    /// Meaning depends on `op`: branch target, constant slot, texture slot,
    /// attribute location, special-register selector.
    std::uint32_t index = 0;
};

struct Block {
    std::vector<Instr> code;
};

struct Function {
    Stage stage = Stage::Vertex;
    std::vector<Block> blocks;
    std::uint8_t gpr_count = 0;
    std::uint8_t predicate_count = 0;
};

/// A shader input or output slot. Location numbering is backend-neutral, which is
/// what lets one module serve both backends.
struct IoSlot {
    std::uint32_t location = 0;
    std::uint8_t components = 4;
    ScalarType type = ScalarType::F32;
    std::string name;  // diagnostics only
};

struct Module {
    Stage stage = Stage::Vertex;
    Function main;
    std::vector<IoSlot> inputs;
    std::vector<IoSlot> outputs;
    /// Structural signature of the source bytes, used as a cache identity.
    std::uint64_t signature = 0;
};

}  // namespace zlong::gpu::shader
