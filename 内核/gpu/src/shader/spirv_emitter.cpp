#include "zlong/gpu/shader/spirv_emitter.h"

#include <cstring>
#include <initializer_list>
namespace zlong::gpu::shader {

namespace {

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::uint32_t kVersion = 0x00010000;

enum : std::uint16_t {
    OpName = 5,
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeImage = 25,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpImageSampleImplicitLod = 87,
    OpConvertFToS = 109,
    OpConvertSToF = 111,
    OpFNegate = 127,
    OpFAdd = 129,
    OpFSub = 131,
    OpFMul = 133,
    OpFDiv = 136,
    OpLabel = 248,
    OpKill = 252,
    OpReturn = 253,
};

constexpr std::uint32_t kCapabilityShader = 1;
constexpr std::uint32_t kStorageUniformConstant = 0;
constexpr std::uint32_t kStorageUniform = 2;
constexpr std::uint32_t kStorageInput = 1;
constexpr std::uint32_t kStorageOutput = 3;
constexpr std::uint32_t kStorageFunction = 7;
constexpr std::uint32_t kDecorationBlock = 2;
constexpr std::uint32_t kDecorationArrayStride = 6;
constexpr std::uint32_t kDecorationBuiltIn = 11;
constexpr std::uint32_t kDecorationLocation = 30;
constexpr std::uint32_t kDecorationBinding = 33;
constexpr std::uint32_t kDecorationDescriptorSet = 34;
constexpr std::uint32_t kDecorationOffset = 35;
constexpr std::uint32_t kBindingConstantBuffer = 0;
/// Texture slot N is bound at kBindingTexture + N, so slot 0 is binding 1 and
/// slot 1 is binding 2 -- the order matches the Vulkan backend's descriptor set.
constexpr std::uint32_t kBindingTexture = 1;
/// Albedo, a normal map, a shadow map.
constexpr std::uint32_t kTextureSlotCount = 3;
/// One kilobyte of constant data: enough for the shaders we emit today.
constexpr std::uint32_t kConstantFloats = 256;
constexpr std::uint32_t kBuiltInPosition = 0;
constexpr std::uint32_t kBuiltInFragCoord = 15;
constexpr std::uint32_t kExecutionModelVertex = 0;
constexpr std::uint32_t kExecutionModelFragment = 4;
constexpr std::uint32_t kExecutionModeOriginUpperLeft = 7;
constexpr std::uint32_t kGlslAbs = 4;
constexpr std::uint32_t kGlslFloor = 8;
constexpr std::uint32_t kGlslFMin = 37;
constexpr std::uint32_t kGlslFMax = 40;
constexpr std::uint32_t kGlslSqrt = 31;

class Ids {
public:
    std::uint32_t New() { return next_++; }
    std::uint32_t bound() const noexcept { return next_; }

private:
    std::uint32_t next_ = 1;
};

/// A contiguous section of the module.
class Bucket {
public:
    void Emit(std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
        words_.push_back((static_cast<std::uint32_t>(operands.size()) + 1u) << 16 | opcode);
        for (const std::uint32_t operand : operands) {
            words_.push_back(operand);
        }
    }

    void Emit(std::uint16_t opcode, const std::vector<std::uint32_t>& operands) {
        words_.push_back((static_cast<std::uint32_t>(operands.size()) + 1u) << 16 | opcode);
        for (const std::uint32_t operand : operands) {
            words_.push_back(operand);
        }
    }

    /// SPIR-V strings are packed into words and must be null terminated. A string
    /// whose length is a multiple of 4 therefore needs an extra all-zero word --
    /// forgetting that leaves the name unterminated and the entry point unfindable.
    void String(std::uint16_t opcode, std::uint32_t id, const char* text) {
        std::vector<std::uint32_t> operands{id};
        const std::size_t length = std::strlen(text);
        const std::size_t word_count = length / 4 + 1;
        operands.reserve(operands.size() + word_count);
        for (std::size_t word = 0; word < word_count; ++word) {
            std::uint32_t packed = 0;
            for (std::size_t byte = 0; byte < 4; ++byte) {
                const std::size_t index = word * 4 + byte;
                if (index < length) {
                    packed |= static_cast<std::uint32_t>(
                                  static_cast<unsigned char>(text[index]))
                              << (8 * byte);
                }
            }
            operands.push_back(packed);
        }
        Emit(opcode, operands);
    }

    const std::vector<std::uint32_t>& words() const noexcept { return words_; }

private:
    std::vector<std::uint32_t> words_;
};

}  // namespace

std::optional<std::vector<std::uint32_t>> EmitSpirv(const Module& module, std::string& error) {
    const Function& function = module.main;
    const bool fragment = module.stage == Stage::Fragment;

    if (function.blocks.size() != 1) {
        error = "the SPIR-V emitter only handles a single basic block so far";
        return std::nullopt;
    }
    if (function.gpr_count == 0) {
        error = "the shader declares no registers";
        return std::nullopt;
    }

    // Which special registers and resources does the shader read?
    bool uses_frag_coord = false;
    bool uses_constants = false;
    bool uses_textures = false;
    bool texture_slot_used[kTextureSlotCount] = {};
    for (const Instr& instruction : function.blocks[0].code) {
        if (instruction.op == Op::ReadSpecial) {
            if (static_cast<Special>(instruction.index) == Special::FragCoord) {
                uses_frag_coord = true;
            } else {
                error = "the SPIR-V emitter only implements the FragCoord special register";
                return std::nullopt;
            }
        }
        if (instruction.op == Op::LoadConst) {
            if (instruction.index != 0) {
                error = "the SPIR-V emitter only implements constant buffer slot 0";
                return std::nullopt;
            }
            uses_constants = true;
        }
        if (instruction.op == Op::SampleTexture || instruction.op == Op::LoadTexture) {
            if (instruction.index >= kTextureSlotCount) {
                error = "the SPIR-V emitter only implements texture slots 0, 1 and 2";
                return std::nullopt;
            }
            uses_textures = true;
            texture_slot_used[instruction.index] = true;
        }
    }

    Ids ids;
    Bucket head;
    Bucket entry;
    Bucket debug;
    Bucket annotations;
    Bucket types;
    Bucket constants;
    Bucket globals;
    Bucket body;

    // ---- ids that must exist before the entry point is written ----------
    const std::uint32_t function_id = ids.New();
    const std::uint32_t label_id = ids.New();

    std::vector<std::uint32_t> input_vars;
    input_vars.reserve(module.inputs.size());
    for (std::size_t i = 0; i < module.inputs.size(); ++i) {
        input_vars.push_back(ids.New());
    }
    std::vector<std::uint32_t> output_vars;
    output_vars.reserve(module.outputs.size());
    for (std::size_t i = 0; i < module.outputs.size(); ++i) {
        output_vars.push_back(ids.New());
    }
    const std::uint32_t frag_coord_var = uses_frag_coord ? ids.New() : 0;
    const std::uint32_t constant_buffer_var = uses_constants ? ids.New() : 0;
    std::uint32_t texture_vars[kTextureSlotCount] = {};
    for (std::uint32_t slot = 0; slot < kTextureSlotCount; ++slot) {
        if (texture_slot_used[slot]) {
            texture_vars[slot] = ids.New();
        }
    }

    std::vector<std::uint32_t> interface_ids;
    for (const std::uint32_t id : input_vars) {
        interface_ids.push_back(id);
    }
    for (const std::uint32_t id : output_vars) {
        interface_ids.push_back(id);
    }
    if (frag_coord_var != 0) {
        interface_ids.push_back(frag_coord_var);
    }
    // The uniform block is deliberately NOT in the interface list: for SPIR-V 1.3
    // and earlier OpEntryPoint interfaces may only be Input/Output variables.

    // ---- head ------------------------------------------------------------
    head.Emit(OpCapability, {kCapabilityShader});
    const std::uint32_t ext_inst = ids.New();
    head.String(OpExtInstImport, ext_inst, "GLSL.std.450");
    head.Emit(OpMemoryModel, {0, 1});  // Logical, GLSL450

    // ---- entry point -----------------------------------------------------
    {
        std::vector<std::uint32_t> operands;
        operands.push_back(fragment ? kExecutionModelFragment : kExecutionModelVertex);
        operands.push_back(function_id);
        operands.push_back(0x6E69'616Du);  // "main", little-endian packed
        operands.push_back(0);             // null terminator (length is a multiple of 4)
        for (const std::uint32_t id : interface_ids) {
            operands.push_back(id);
        }
        entry.Emit(OpEntryPoint, operands);
        if (fragment) {
            entry.Emit(OpExecutionMode, {function_id, kExecutionModeOriginUpperLeft});
        }
    }

    // ---- names + decorations --------------------------------------------
    debug.String(OpName, function_id, "main");
    for (std::size_t i = 0; i < module.inputs.size(); ++i) {
        debug.String(OpName, input_vars[i],
                     module.inputs[i].name.empty() ? "input" : module.inputs[i].name.c_str());
        annotations.Emit(OpDecorate, {input_vars[i], kDecorationLocation, module.inputs[i].location});
    }
    for (std::size_t i = 0; i < module.outputs.size(); ++i) {
        const bool position = !fragment && i == 0;
        debug.String(OpName, output_vars[i],
                     position ? "gl_Position"
                              : (module.outputs[i].name.empty() ? "output"
                                                                : module.outputs[i].name.c_str()));
        if (position) {
            annotations.Emit(OpDecorate, {output_vars[i], kDecorationBuiltIn, kBuiltInPosition});
        } else {
            annotations.Emit(OpDecorate,
                             {output_vars[i], kDecorationLocation, module.outputs[i].location});
        }
    }
    if (frag_coord_var != 0) {
        debug.String(OpName, frag_coord_var, "gl_FragCoord");
        annotations.Emit(OpDecorate, {frag_coord_var, kDecorationBuiltIn, kBuiltInFragCoord});
    }

    // ---- types -----------------------------------------------------------
    const std::uint32_t void_type = ids.New();
    types.Emit(OpTypeVoid, {void_type});
    const std::uint32_t float_type = ids.New();
    types.Emit(OpTypeFloat, {float_type, 32});
    const std::uint32_t vec4_type = ids.New();
    types.Emit(OpTypeVector, {vec4_type, float_type, 4});
    const std::uint32_t vec2_type = ids.New();
    types.Emit(OpTypeVector, {vec2_type, float_type, 2});
    const std::uint32_t int_type = ids.New();
    types.Emit(OpTypeInt, {int_type, 32, 1});
    const std::uint32_t ptr_function_float = ids.New();
    types.Emit(OpTypePointer, {ptr_function_float, kStorageFunction, float_type});
    const std::uint32_t ptr_input_vec4 = ids.New();
    types.Emit(OpTypePointer, {ptr_input_vec4, kStorageInput, vec4_type});
    const std::uint32_t ptr_output_vec4 = ids.New();
    types.Emit(OpTypePointer, {ptr_output_vec4, kStorageOutput, vec4_type});
    const std::uint32_t function_type = ids.New();
    types.Emit(OpTypeFunction, {function_type, void_type});

    // ---- constant buffer (uniform block of floats) -----------------------
    std::uint32_t ptr_uniform_vec4 = 0;
    std::uint32_t uint_type_id = 0;
    if (uses_constants) {
        const std::uint32_t uint_type = ids.New();
        uint_type_id = uint_type;
        types.Emit(OpTypeInt, {uint_type, 32, 0});
        const std::uint32_t array_length = ids.New();
        // The array type references this constant, so it has to be emitted in the
        // same section, before it.
        types.Emit(OpConstant, {uint_type, array_length, kConstantFloats});

        // A float array with stride 4 is not legal in a uniform block, so the
        // block is an array of vec4 (stride 16) and reads are vec4-granular.
        const std::uint32_t vec4_array = ids.New();
        types.Emit(OpTypeArray, {vec4_array, vec4_type, array_length});
        annotations.Emit(OpDecorate, {vec4_array, kDecorationArrayStride, 16});

        const std::uint32_t block = ids.New();
        types.Emit(OpTypeStruct, {block, vec4_array});
        annotations.Emit(OpDecorate, {block, kDecorationBlock});
        annotations.Emit(OpMemberDecorate, {block, 0, kDecorationOffset, 0});

        const std::uint32_t ptr_uniform_block = ids.New();
        types.Emit(OpTypePointer, {ptr_uniform_block, kStorageUniform, block});
        ptr_uniform_vec4 = ids.New();
        types.Emit(OpTypePointer, {ptr_uniform_vec4, kStorageUniform, vec4_type});

        annotations.Emit(
            OpDecorate,
            {constant_buffer_var, kDecorationDescriptorSet, 0});
        annotations.Emit(
            OpDecorate,
            {constant_buffer_var, kDecorationBinding, kBindingConstantBuffer});
        globals.Emit(OpVariable, {ptr_uniform_block, constant_buffer_var, kStorageUniform});
    }

    // ---- combined image sampler -----------------------------------------
    std::uint32_t sampled_image_type = 0;
    if (uses_textures) {
        const std::uint32_t image_type = ids.New();
        // OpTypeImage: sampled type, Dim=2D, Depth=0, Arrayed=0, MS=0, Sampled=1,
        // format Unknown.
        types.Emit(OpTypeImage, {image_type, float_type, 1, 0, 0, 0, 1, 0});
        sampled_image_type = ids.New();
        types.Emit(OpTypeSampledImage, {sampled_image_type, image_type});
        const std::uint32_t ptr_sampled = ids.New();
        types.Emit(OpTypePointer, {ptr_sampled, kStorageUniformConstant, sampled_image_type});
        for (std::uint32_t slot = 0; slot < kTextureSlotCount; ++slot) {
            if (texture_vars[slot] == 0) {
                continue;
            }
            annotations.Emit(OpDecorate, {texture_vars[slot], kDecorationDescriptorSet, 0});
            annotations.Emit(
                OpDecorate, {texture_vars[slot], kDecorationBinding, kBindingTexture + slot});
            globals.Emit(OpVariable, {ptr_sampled, texture_vars[slot], kStorageUniformConstant});
        }
    }

    // ---- global (interface) variables -----------------------------------
    for (const std::uint32_t id : input_vars) {
        globals.Emit(OpVariable, {ptr_input_vec4, id, kStorageInput});
    }
    for (const std::uint32_t id : output_vars) {
        globals.Emit(OpVariable, {ptr_output_vec4, id, kStorageOutput});
    }
    if (frag_coord_var != 0) {
        globals.Emit(OpVariable, {ptr_input_vec4, frag_coord_var, kStorageInput});
    }

    // ---- bodies ----------------------------------------------------------
    std::vector<std::pair<float, std::uint32_t>> float_constants;
    const auto float_constant = [&](float value) {
        for (const auto& existing : float_constants) {
            if (existing.first == value) {
                return existing.second;
            }
        }
        const std::uint32_t id = ids.New();
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        constants.Emit(OpConstant, {float_type, id, bits});
        float_constants.emplace_back(value, id);
        return id;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> uint_constants;
    const auto uint_constant = [&](std::uint32_t value) {
        for (const auto& existing : uint_constants) {
            if (existing.first == value) {
                return existing.second;
            }
        }
        const std::uint32_t id = ids.New();
        constants.Emit(OpConstant, {uint_type_id, id, value});
        uint_constants.emplace_back(value, id);
        return id;
    };

    std::vector<std::uint32_t> lane_vars(static_cast<std::size_t>(function.gpr_count) * 4, 0);
    // A register outside the shader's own file is the zero register: it reads zero
    // and a write to it goes nowhere. The interpreter gets that by checking the index
    // against its own vector; these two have to check too rather than index past the
    // end, which is where a decoded RZ used to land.
    const auto load_lane = [&](int gpr, int lane) {
        if (gpr >= function.gpr_count) {
            return float_constant(0.0f);
        }
        const std::uint32_t id = ids.New();
        body.Emit(OpLoad, {float_type, id, lane_vars[static_cast<std::size_t>(gpr) * 4 + lane]});
        return id;
    };
    const auto store_lane = [&](int gpr, int lane, std::uint32_t value) {
        if (gpr >= function.gpr_count) {
            return;
        }
        body.Emit(OpStore, {lane_vars[static_cast<std::size_t>(gpr) * 4 + lane], value});
    };

    body.Emit(OpFunction, {void_type, function_id, 0, function_type});
    body.Emit(OpLabel, {label_id});

    // Spec: every OpVariable with Function storage must be the first
    // instructions of the entry block, with no other instruction interleaved.
    const std::uint32_t zero = float_constant(0.0f);
    for (std::size_t i = 0; i < lane_vars.size(); ++i) {
        lane_vars[i] = ids.New();
    }
    for (const std::uint32_t variable : lane_vars) {
        body.Emit(OpVariable, {ptr_function_float, variable, kStorageFunction});
    }
    for (const std::uint32_t variable : lane_vars) {
        body.Emit(OpStore, {variable, zero});
    }

    const auto emit_read_vec4 = [&](std::uint32_t pointer, int dst_gpr) {
        const std::uint32_t loaded = ids.New();
        body.Emit(OpLoad, {vec4_type, loaded, pointer});
        for (int lane = 0; lane < 4; ++lane) {
            const std::uint32_t extracted = ids.New();
            body.Emit(OpCompositeExtract,
                      {float_type, extracted, loaded, static_cast<std::uint32_t>(lane)});
            store_lane(dst_gpr, lane, extracted);
        }
    };

    for (const Instr& instruction : function.blocks[0].code) {
        const int dst = instruction.dst.index;
        switch (instruction.op) {
        case Op::Nop:
        case Op::Label:
            break;

        case Op::Return:
            body.Emit(OpReturn, {});
            break;
        case Op::Kill:
            body.Emit(OpKill, {});
            break;

        case Op::Mov: {
            const int src = instruction.src[0].index;
            for (int lane = 0; lane < 4; ++lane) {
                store_lane(dst, lane, load_lane(src, lane));
            }
            break;
        }
        case Op::LoadImm:
            if (instruction.dst.component >= 4) {
                error = "load-immediate lane out of range";
                return std::nullopt;
            }
            store_lane(dst, instruction.dst.component, float_constant(instruction.imm_f));
            break;

        case Op::LoadAttribute:
        case Op::Interpolate:
            if (instruction.index >= input_vars.size()) {
                error = "shader input slot out of range";
                return std::nullopt;
            }
            emit_read_vec4(input_vars[instruction.index], dst);
            break;

        case Op::SampleTexture:
        case Op::LoadTexture: {
            if (sampled_image_type == 0 || instruction.index >= kTextureSlotCount ||
                texture_vars[instruction.index] == 0) {
                error = "texture used but not declared";
                return std::nullopt;
            }
            const int uv_register = instruction.src[0].index;
            const std::uint32_t loaded = ids.New();
            body.Emit(OpLoad, {sampled_image_type, loaded, texture_vars[instruction.index]});
            const std::uint32_t uv = ids.New();
            body.Emit(OpCompositeConstruct,
                      {vec2_type, uv, load_lane(uv_register, 0), load_lane(uv_register, 1)});
            const std::uint32_t sampled = ids.New();
            body.Emit(OpImageSampleImplicitLod, {vec4_type, sampled, loaded, uv});
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t extracted = ids.New();
                body.Emit(OpCompositeExtract,
                          {float_type, extracted, sampled, static_cast<std::uint32_t>(lane)});
                store_lane(dst, lane, extracted);
            }
            break;
        }

        case Op::ReadSpecial:
            emit_read_vec4(frag_coord_var, dst);
            break;

        case Op::LoadConst: {
            if (ptr_uniform_vec4 == 0) {
                error = "constant buffer used but not declared";
                return std::nullopt;
            }
            if ((instruction.imm_u % 16) != 0) {
                error = "constant buffer reads must be 16-byte aligned";
                return std::nullopt;
            }
            const std::uint32_t element = ids.New();
            body.Emit(OpAccessChain, {ptr_uniform_vec4, element, constant_buffer_var,
                                      uint_constant(0), uint_constant(instruction.imm_u / 16)});
            const std::uint32_t loaded = ids.New();
            body.Emit(OpLoad, {vec4_type, loaded, element});
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t extracted = ids.New();
                body.Emit(OpCompositeExtract,
                          {float_type, extracted, loaded, static_cast<std::uint32_t>(lane)});
                store_lane(dst, lane, extracted);
            }
            break;
        }

        case Op::StoreOutput: {
            if (instruction.index >= output_vars.size()) {
                error = "shader output slot out of range";
                return std::nullopt;
            }
            const int src = instruction.src[0].index;
            const std::uint32_t built = ids.New();
            body.Emit(OpCompositeConstruct,
                      {vec4_type, built, load_lane(src, 0), load_lane(src, 1), load_lane(src, 2),
                       load_lane(src, 3)});
            body.Emit(OpStore, {output_vars[instruction.index], built});
            break;
        }

        case Op::FMin:
        case Op::FMax: {
            // GLSL.std.450, like Abs/Floor/Sqrt above: the extension is already
            // imported and its FMin/FMax are the pair the interpreter means.
            //
            // Only the two clamping ops live here. FAdd and FSub must NOT be folded
            // into this case: they have no GLSL.std.450 spelling, and a selector
            // that only distinguishes FMin from "everything else" compiles an
            // addition into an FMax -- which silently turns `ambient + direct` into
            // `max(ambient, direct)` on the Vulkan path alone.
            const std::uint32_t which = instruction.op == Op::FMin ? kGlslFMin : kGlslFMax;
            const int a = instruction.src[0].index;
            const int b = instruction.src[1].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t id = ids.New();
                body.Emit(OpExtInst,
                          {float_type, id, ext_inst, which, load_lane(a, lane), load_lane(b, lane)});
                store_lane(dst, lane, id);
            }
            break;
        }
        case Op::FAdd:
        case Op::FSub:
        case Op::FMul: {
            const std::uint16_t opcode =
                instruction.op == Op::FAdd ? OpFAdd : (instruction.op == Op::FSub ? OpFSub : OpFMul);
            const int a = instruction.src[0].index;
            const int b = instruction.src[1].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t id = ids.New();
                body.Emit(opcode, {float_type, id, load_lane(a, lane), load_lane(b, lane)});
                store_lane(dst, lane, id);
            }
            break;
        }
        case Op::FDot: {
            if (instruction.dst.component >= 4) {
                error = "dot destination lane out of range";
                return std::nullopt;
            }
            const int a = instruction.src[0].index;
            const int b = instruction.src[1].index;
            std::uint32_t sum = 0;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t product = ids.New();
                body.Emit(OpFMul, {float_type, product, load_lane(a, lane), load_lane(b, lane)});
                if (lane == 0) {
                    sum = product;
                } else {
                    const std::uint32_t next = ids.New();
                    body.Emit(OpFAdd, {float_type, next, sum, product});
                    sum = next;
                }
            }
            store_lane(dst, instruction.dst.component, sum);
            break;
        }
        case Op::Splat: {
            if (instruction.src[0].component >= 4) {
                error = "splat lane out of range";
                return std::nullopt;
            }
            const std::uint32_t scalar = load_lane(instruction.src[0].index,
                                                   instruction.src[0].component);
            for (int lane = 0; lane < 4; ++lane) {
                store_lane(dst, lane, scalar);
            }
            break;
        }
        case Op::FFma: {
            const int a = instruction.src[0].index;
            const int b = instruction.src[1].index;
            const int c = instruction.src[2].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t mul = ids.New();
                body.Emit(OpFMul, {float_type, mul, load_lane(a, lane), load_lane(b, lane)});
                const std::uint32_t add = ids.New();
                body.Emit(OpFAdd, {float_type, add, mul, load_lane(c, lane)});
                store_lane(dst, lane, add);
            }
            break;
        }
        case Op::FNeg: {
            const int a = instruction.src[0].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t id = ids.New();
                body.Emit(OpFNegate, {float_type, id, load_lane(a, lane)});
                store_lane(dst, lane, id);
            }
            break;
        }
        case Op::FAbs:
        case Op::FFloor:
        case Op::FSqrt: {
            const std::uint32_t which = instruction.op == Op::FAbs ? kGlslAbs
                                        : instruction.op == Op::FFloor ? kGlslFloor
                                                                       : kGlslSqrt;
            const int a = instruction.src[0].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t id = ids.New();
                body.Emit(OpExtInst, {float_type, id, ext_inst, which, load_lane(a, lane)});
                store_lane(dst, lane, id);
            }
            break;
        }
        case Op::FRcp: {
            const std::uint32_t one = float_constant(1.0f);
            const int a = instruction.src[0].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t id = ids.New();
                body.Emit(OpFDiv, {float_type, id, one, load_lane(a, lane)});
                store_lane(dst, lane, id);
            }
            break;
        }
        case Op::F2F: {
            const int a = instruction.src[0].index;
            for (int lane = 0; lane < 4; ++lane) {
                store_lane(dst, lane, load_lane(a, lane));
            }
            break;
        }
        case Op::F2I:
        case Op::I2F: {
            const int a = instruction.src[0].index;
            for (int lane = 0; lane < 4; ++lane) {
                const std::uint32_t as_int = ids.New();
                body.Emit(OpConvertFToS, {int_type, as_int, load_lane(a, lane)});
                const std::uint32_t back = ids.New();
                body.Emit(OpConvertSToF, {float_type, back, as_int});
                store_lane(dst, lane, back);
            }
            break;
        }

        default:
            error = "the SPIR-V emitter does not implement this instruction yet";
            return std::nullopt;
        }
    }

    body.Emit(OpFunctionEnd, {});

    // ---- assemble --------------------------------------------------------
    std::vector<std::uint32_t> out;
    out.push_back(kMagic);
    out.push_back(kVersion);
    out.push_back(0);  // generator
    out.push_back(ids.bound());
    out.push_back(0);  // schema

    const auto append = [&out](const Bucket& bucket) {
        out.insert(out.end(), bucket.words().begin(), bucket.words().end());
    };
    append(head);
    append(entry);
    append(debug);
    append(annotations);
    append(types);
    append(constants);
    append(globals);
    append(body);

    error.clear();
    return out;
}

}  // namespace zlong::gpu::shader
