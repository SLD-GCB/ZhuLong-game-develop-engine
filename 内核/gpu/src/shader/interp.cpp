#include "zlong/gpu/shader/interp.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "zlong/gpu/surface/format.h"
#include "zlong/gpu/surface/layout.h"
#include "zlong/gpu/surface/texel.h"

namespace zlong::gpu::shader {

namespace {

using Vec4 = std::array<float, 4>;

/// Runaway guard: a shader loop that never terminates must not hang the host.
constexpr std::size_t kStepLimit = 1u << 20;

bool CompareF(Compare op, float a, float b) {
    switch (op) {
    case Compare::Never: return false;
    case Compare::Less: return a < b;
    case Compare::Equal: return a == b;
    case Compare::LessEqual: return a <= b;
    case Compare::Greater: return a > b;
    case Compare::NotEqual: return a != b;
    case Compare::GreaterEqual: return a >= b;
    case Compare::Always: return true;
    }
    return false;
}

int WrapCoord(SamplerState::Wrap wrap, int coord, std::uint32_t extent) {
    if (extent == 0) {
        return 0;
    }
    const int n = static_cast<int>(extent);
    switch (wrap) {
    case SamplerState::Wrap::Repeat: {
        int m = coord % n;
        return m < 0 ? m + n : m;
    }
    case SamplerState::Wrap::Mirror: {
        const int period = 2 * n;
        int m = coord % period;
        if (m < 0) {
            m += period;
        }
        return m < n ? m : period - 1 - m;
    }
    case SamplerState::Wrap::Clamp:
    default:
        return coord < 0 ? 0 : (coord >= n ? n - 1 : coord);
    }
}

bool FetchTexel(const TextureRef& texture, const MemorySource& memory, int x, int y, Vec4& out) {
    const surface::FormatInfo info = surface::DecodeFormat(texture.format);
    if (info.bytes_per_pixel == 0 || texture.width == 0 || texture.height == 0) {
        return false;
    }
    surface::Surface surface;
    surface.width = texture.width;
    surface.height = texture.height;
    surface.pitch = texture.pitch;
    surface.tile = texture.tile;
    surface.bpp = info.bytes_per_pixel;

    const std::size_t offset = surface::OffsetOf(surface, static_cast<std::uint32_t>(x),
                                                 static_cast<std::uint32_t>(y));
    if (offset == surface::kInvalidOffset) {
        return false;
    }
    const std::uint8_t* bytes = memory.peek(texture.pa + offset, info.bytes_per_pixel);
    if (bytes == nullptr) {
        return false;
    }
    out = surface::DecodeTexel(info, bytes);
    return true;
}

/// SASS local storage is addressed, so keep it sparse.
class LocalMemory {
public:
    Vec4& at(std::uint32_t index) {
        if (index >= slots_.size()) {
            slots_.resize(static_cast<std::size_t>(index) + 1, Vec4{0.0f, 0.0f, 0.0f, 0.0f});
        }
        return slots_[index];
    }

private:
    std::vector<Vec4> slots_;
};

}  // namespace

InterpResult Interpret(const Module& module, const ShaderIo& in, ShaderIo& out) {
    InterpResult result;

    const Function& function = module.main;
    if (function.blocks.empty()) {
        result.error = "shader module has no blocks";
        return result;
    }

    // Flatten to one instruction stream, recording where each label lands.
    std::vector<const Instr*> code;
    std::unordered_map<std::uint32_t, std::size_t> labels;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.code) {
            if (instruction.op == Op::Label) {
                labels[instruction.index] = code.size();
            }
            code.push_back(&instruction);
        }
    }

    std::vector<Vec4> gpr(function.gpr_count == 0 ? 64 : function.gpr_count, Vec4{0, 0, 0, 0});
    std::vector<bool> pred(function.predicate_count == 0 ? 8 : function.predicate_count, false);
    LocalMemory local;

    if (out.outputs.size() < module.outputs.size()) {
        out.outputs.resize(module.outputs.size(), Vec4{0, 0, 0, 0});
    }

    const MemorySource* memory = in.memory;

    std::size_t pc = 0;
    std::size_t steps = 0;

    while (pc < code.size()) {
        if (++steps > kStepLimit) {
            result.error = "shader exceeded the instruction budget";
            return result;
        }
        const Instr& instruction = *code[pc];
        std::size_t next = pc + 1;

        if (instruction.predicated) {
            bool value = instruction.predicate.index < pred.size()
                             ? pred[instruction.predicate.index]
                             : false;
            if (instruction.pred_negate) {
                value = !value;
            }
            if (!value) {
                pc = next;
                continue;
            }
        }

        const auto read_src = [&](std::size_t index) -> Vec4 {
            const Value& value = instruction.src[index];
            if (value.kind == RegKind::Predicate) {
                const float f =
                    (value.index < pred.size() && pred[value.index]) ? 1.0f : 0.0f;
                return Vec4{f, f, f, f};
            }
            return value.index < gpr.size() ? gpr[value.index] : Vec4{0, 0, 0, 0};
        };
        const auto write_dst = [&](const Vec4& value) {
            if (instruction.dst.kind == RegKind::Predicate) {
                if (instruction.dst.index < pred.size()) {
                    pred[instruction.dst.index] = value[0] != 0.0f;
                }
                return;
            }
            if (instruction.dst.index < gpr.size()) {
                gpr[instruction.dst.index] = value;
            }
        };
        const auto binary_float = [&](auto fn) {
            const Vec4 a = read_src(0);
            const Vec4 b = read_src(1);
            Vec4 r{};
            for (int lane = 0; lane < 4; ++lane) {
                r[lane] = fn(a[lane], b[lane]);
            }
            write_dst(r);
        };
        const auto unary_float = [&](auto fn) {
            const Vec4 a = read_src(0);
            Vec4 r{};
            for (int lane = 0; lane < 4; ++lane) {
                r[lane] = fn(a[lane]);
            }
            write_dst(r);
        };
        const auto binary_bits = [&](auto fn) {
            const Vec4 a = read_src(0);
            const Vec4 b = read_src(1);
            Vec4 r{};
            for (int lane = 0; lane < 4; ++lane) {
                std::uint32_t x = 0;
                std::uint32_t y = 0;
                std::memcpy(&x, &a[lane], 4);
                std::memcpy(&y, &b[lane], 4);
                const std::uint32_t z = fn(x, y);
                std::memcpy(&r[lane], &z, 4);
            }
            write_dst(r);
        };

        switch (instruction.op) {
        case Op::Nop:
        case Op::Label:
            break;

        case Op::Mov:
            write_dst(read_src(0));
            break;
        case Op::LoadImm: {
            // Writes a single lane, so a vec4 constant is built lane by lane.
            Vec4 value = instruction.dst.index < gpr.size() ? gpr[instruction.dst.index]
                                                            : Vec4{0, 0, 0, 0};
            if (instruction.dst.component < 4) {
                value[instruction.dst.component] = instruction.imm_f;
            }
            write_dst(value);
            break;
        }
        case Op::ReadSpecial:
            if (instruction.index >= in.special.size()) {
                result.error = "special register out of range";
                return result;
            }
            write_dst(in.special[instruction.index]);
            break;
        case Op::LoadAttribute:
        case Op::Interpolate:
            if (instruction.index >= in.inputs.size()) {
                result.error = "shader input slot out of range";
                return result;
            }
            write_dst(in.inputs[instruction.index]);
            break;
        case Op::StoreOutput:
            if (instruction.index >= out.outputs.size()) {
                result.error = "shader output slot out of range";
                return result;
            }
            out.outputs[instruction.index] = read_src(0);
            break;

        case Op::Branch: {
            const auto it = labels.find(instruction.index);
            if (it == labels.end()) {
                result.error = "branch to an unknown label";
                return result;
            }
            next = it->second;
            break;
        }
        case Op::BranchConditional: {
            const Value& condition = instruction.src[0];
            const bool taken =
                condition.kind == RegKind::Predicate && condition.index < pred.size() &&
                pred[condition.index];
            if (taken) {
                const auto it = labels.find(instruction.index);
                if (it == labels.end()) {
                    result.error = "branch to an unknown label";
                    return result;
                }
                next = it->second;
            }
            break;
        }
        case Op::Return:
            pc = code.size();
            continue;
        case Op::Kill:
            result.killed = true;
            pc = code.size();
            continue;

        case Op::FSetP: {
            const Vec4 a = read_src(0);
            const Vec4 b = read_src(1);
            if (instruction.dst.index < pred.size()) {
                pred[instruction.dst.index] =
                    CompareF(static_cast<Compare>(instruction.index), a[0], b[0]);
            }
            break;
        }
        case Op::ISetP: {
            const Vec4 a = read_src(0);
            const Vec4 b = read_src(1);
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::memcpy(&x, &a[0], 4);
            std::memcpy(&y, &b[0], 4);
            const Compare op = static_cast<Compare>(instruction.index);
            const bool value = [&] {
                switch (op) {
                case Compare::Never: return false;
                case Compare::Less: return x < y;
                case Compare::Equal: return x == y;
                case Compare::LessEqual: return x <= y;
                case Compare::Greater: return x > y;
                case Compare::NotEqual: return x != y;
                case Compare::GreaterEqual: return x >= y;
                case Compare::Always: return true;
                }
                return false;
            }();
            if (instruction.dst.index < pred.size()) {
                pred[instruction.dst.index] = value;
            }
            break;
        }
        case Op::SetP:
            write_dst(read_src(0));
            break;

        case Op::FAdd:
            binary_float([](float a, float b) { return a + b; });
            break;
        case Op::FSub:
            binary_float([](float a, float b) { return a - b; });
            break;
        case Op::FMul:
            binary_float([](float a, float b) { return a * b; });
            break;
        case Op::FFma: {
            const Vec4 a = read_src(0);
            const Vec4 b = read_src(1);
            const Vec4 c = read_src(2);
            Vec4 r{};
            for (int lane = 0; lane < 4; ++lane) {
                r[lane] = a[lane] * b[lane] + c[lane];
            }
            write_dst(r);
            break;
        }
        case Op::FDot: {
            const Vec4 a = read_src(0);
            const Vec4 b = read_src(1);
            float sum = 0.0f;
            for (int lane = 0; lane < 4; ++lane) {
                sum += a[lane] * b[lane];
            }
            // One lane, like LoadImm: a scalar destination selects a lane.
            Vec4 value = instruction.dst.index < gpr.size() ? gpr[instruction.dst.index]
                                                            : Vec4{0, 0, 0, 0};
            if (instruction.dst.component < 4) {
                value[instruction.dst.component] = sum;
            }
            write_dst(value);
            break;
        }
        case Op::Splat: {
            const Vec4 source = read_src(0);
            const int lane = instruction.src[0].component < 4 ? instruction.src[0].component : 0;
            write_dst(Vec4{source[lane], source[lane], source[lane], source[lane]});
            break;
        }
        case Op::FMin:
            binary_float([](float a, float b) { return a < b ? a : b; });
            break;
        case Op::FMax:
            binary_float([](float a, float b) { return a > b ? a : b; });
            break;
        case Op::FNeg:
            unary_float([](float a) { return -a; });
            break;
        case Op::FAbs:
            unary_float([](float a) { return std::fabs(a); });
            break;
        case Op::FFloor:
            unary_float([](float a) { return std::floor(a); });
            break;
        case Op::FRcp:
            unary_float([](float a) { return a != 0.0f ? 1.0f / a : 0.0f; });
            break;
        case Op::FSqrt:
            unary_float([](float a) { return a > 0.0f ? std::sqrt(a) : 0.0f; });
            break;
        case Op::F2F:
            unary_float([](float a) { return a; });
            break;
        case Op::F2I:
            unary_float([](float a) { return static_cast<float>(static_cast<std::int32_t>(a)); });
            break;
        case Op::I2F:
            unary_float([](float a) { return static_cast<float>(static_cast<std::int32_t>(a)); });
            break;

        case Op::IAdd:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a + b; });
            break;
        case Op::ISub:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a - b; });
            break;
        case Op::IMul:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a * b; });
            break;
        case Op::IAnd:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a & b; });
            break;
        case Op::IOr:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a | b; });
            break;
        case Op::IXor:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a ^ b; });
            break;
        case Op::IShl:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a << (b & 31u); });
            break;
        case Op::IShr:
            binary_bits([](std::uint32_t a, std::uint32_t b) { return a >> (b & 31u); });
            break;
        case Op::Lop3:
            // A three-input logic op with a truth-table immediate: reporting it
            // is honest, silently returning the first operand is not.
            result.error = "Lop3 is not implemented yet";
            return result;

        case Op::LoadConst: {
            if (in.constants == nullptr || memory == nullptr) {
                result.error = "no constant buffers bound";
                return result;
            }
            const ConstantBufferBinding* binding = nullptr;
            for (const auto& candidate : *in.constants) {
                if (candidate.slot == instruction.index) {
                    binding = &candidate;
                    break;
                }
            }
            if (binding == nullptr) {
                result.error = "constant buffer slot is not bound";
                return result;
            }
            const std::uint64_t offset = instruction.imm_u;
            if (offset + 16 > binding->size) {
                result.error = "constant buffer read out of range";
                return result;
            }
            const std::uint8_t* bytes = memory->peek(binding->pa + offset, 16);
            if (bytes == nullptr) {
                result.error = "constant buffer is not mapped";
                return result;
            }
            Vec4 value{};
            std::memcpy(value.data(), bytes, 16);
            write_dst(value);
            break;
        }
        case Op::LoadLocal:
            write_dst(local.at(instruction.imm_u));
            break;
        case Op::StoreLocal:
            local.at(instruction.imm_u) = read_src(0);
            break;
        case Op::LoadGlobal:
        case Op::StoreGlobal:
            result.error = "global memory access is not implemented yet";
            return result;

        case Op::LoadTexture: {
            if (in.textures == nullptr || memory == nullptr) {
                result.error = "no textures bound";
                return result;
            }
            const TextureBinding* binding = nullptr;
            for (const auto& candidate : *in.textures) {
                if (candidate.slot == instruction.index) {
                    binding = &candidate;
                    break;
                }
            }
            if (binding == nullptr) {
                result.error = "texture slot is not bound";
                return result;
            }
            const Vec4 coord = read_src(0);
            Vec4 texel{};
            if (!FetchTexel(binding->texture, *memory, static_cast<int>(coord[0]),
                            static_cast<int>(coord[1]), texel)) {
                result.error = "texture fetch failed";
                return result;
            }
            write_dst(texel);
            break;
        }
        case Op::SampleTexture: {
            if (in.textures == nullptr || memory == nullptr) {
                result.error = "no textures bound";
                return result;
            }
            const TextureBinding* binding = nullptr;
            for (const auto& candidate : *in.textures) {
                if (candidate.slot == instruction.index) {
                    binding = &candidate;
                    break;
                }
            }
            if (binding == nullptr) {
                result.error = "texture slot is not bound";
                return result;
            }
            const Vec4 uv = read_src(0);
            const bool nearest = binding->sampler.min_filter == SamplerState::Filter::Nearest &&
                                 binding->sampler.mag_filter == SamplerState::Filter::Nearest;

            Vec4 texel{};
            if (nearest) {
                const int x = static_cast<int>(std::floor(
                    uv[0] * static_cast<float>(binding->texture.width)));
                const int y = static_cast<int>(std::floor(
                    uv[1] * static_cast<float>(binding->texture.height)));
                if (!FetchTexel(binding->texture, *memory,
                                WrapCoord(binding->sampler.wrap_u, x, binding->texture.width),
                                WrapCoord(binding->sampler.wrap_v, y, binding->texture.height),
                                texel)) {
                    result.error = "texture sample failed";
                    return result;
                }
            } else {
                // Bilinear: sample the four neighbours around the texel centre and
                // lerp. Texel centres sit at (i + 0.5), hence the half-texel shift.
                const float fx = uv[0] * static_cast<float>(binding->texture.width) - 0.5f;
                const float fy = uv[1] * static_cast<float>(binding->texture.height) - 0.5f;
                const int x0 = static_cast<int>(std::floor(fx));
                const int y0 = static_cast<int>(std::floor(fy));
                const float tx = fx - static_cast<float>(x0);
                const float ty = fy - static_cast<float>(y0);

                Vec4 corners[4]{};
                for (int index = 0; index < 4; ++index) {
                    const int offset_x = index & 1;
                    const int offset_y = (index >> 1) & 1;
                    if (!FetchTexel(
                            binding->texture, *memory,
                            WrapCoord(binding->sampler.wrap_u, x0 + offset_x,
                                      binding->texture.width),
                            WrapCoord(binding->sampler.wrap_v, y0 + offset_y,
                                      binding->texture.height),
                            corners[index])) {
                        result.error = "texture sample failed";
                        return result;
                    }
                }
                for (int lane = 0; lane < 4; ++lane) {
                    const float top = corners[0][lane] +
                                      (corners[1][lane] - corners[0][lane]) * tx;
                    const float bottom = corners[2][lane] +
                                         (corners[3][lane] - corners[2][lane]) * tx;
                    texel[lane] = top + (bottom - top) * ty;
                }
            }
            write_dst(texel);
            break;
        }
        }

        pc = next;
    }

    return result;
}

}  // namespace zlong::gpu::shader
