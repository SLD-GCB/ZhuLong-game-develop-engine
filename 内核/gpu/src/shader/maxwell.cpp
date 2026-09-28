#include "zlong/gpu/shader/maxwell.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "zlong/gpu/shader/maxwell_encoding.h"

namespace zlong::gpu::shader {

namespace {

// Every encoding constant comes from shader/maxwell_encoding.h, which is
// transcribed from envytools' envydis/gm107.c -- read its provenance note.
// Nothing here hard-codes a bit position.
constexpr std::size_t kInstructionBytes = kFormat.instruction_bytes;
constexpr std::size_t kHeaderBytes = kShaderHeaderBytes;

/// One IR IoSlot is four F32 components, so a slot is 16 bytes of attribute or
/// output space. Encoded offsets are byte offsets into that space.
constexpr std::uint32_t kIoSlotBytes = 16;

std::uint32_t Field(std::uint64_t word, std::uint32_t bit, std::uint32_t bits) {
    return static_cast<std::uint32_t>((word >> bit) & ((1ull << bits) - 1ull));
}

std::uint32_t SignExtend(std::uint64_t field, std::uint32_t bits) {
    const std::uint64_t sign = 1ull << (bits - 1);
    return static_cast<std::uint32_t>(((field ^ sign) - sign) & 0xFFFF'FFFFull);
}

float FloatOf(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Value Gpr(std::uint32_t index) {
    return Value{RegKind::Gpr, static_cast<std::uint8_t>(index & 0xFFu), 0};
}

/// The opcode and its operands live in the low 64-bit word of the instruction.
/// Guest and host are both little-endian.
std::uint64_t ReadInstruction(const std::uint8_t* bytes) {
    std::uint64_t word = 0;
    std::memcpy(&word, bytes, sizeof(word));
    return word;
}

/// gm107.c s2020_bf{{20,19,56,1}}: a 20-bit signed value whose top bit is bit
/// 56 rather than bit 39. That is why bit 56 is outside the opcode mask.
std::uint32_t ShortImmediate(std::uint64_t word) {
    const std::uint64_t low = Field(word, kFormat.imm20_bit, kFormat.imm20_low_bits);
    const std::uint64_t high = Field(word, kFormat.imm20_high_bit, 1);
    return SignExtend(low | (high << kFormat.imm20_low_bits), kFormat.imm20_low_bits + 1);
}

/// gm107.c f1920_bf (.shr = 12): the field is the top 20 bits of a float.
float FloatImmediate(std::uint64_t word) {
    return FloatOf(ShortImmediate(word) << kFormat.float20_shift);
}

/// The guard predicate, gm107.c tabpred[]: bits 16..19, 0b0111 meaning "no
/// guard" and 0b1111 "never".
struct Guard {
    bool predicated = false;
    Value predicate{};
    bool negate = false;
};

bool ReadGuard(std::uint64_t word, Guard& guard, std::string& error) {
    const std::uint32_t raw = Field(word, kFormat.guard_bit, kFormat.guard_bits);
    if (raw == kFormat.guard_bare) {
        return true;
    }
    if (raw == kFormat.guard_never) {
        error = "the shader uses an always-false predicate";
        return false;
    }
    guard.predicated = true;
    guard.predicate = Value{RegKind::Predicate, static_cast<std::uint8_t>(raw & 0x7u), 0};
    guard.negate = (raw & 0x8u) != 0;
    return true;
}

void ApplyGuard(const Guard& guard, Instr& instruction) {
    if (!guard.predicated) {
        return;
    }
    instruction.predicated = true;
    instruction.predicate = guard.predicate;
    instruction.pred_negate = guard.negate;
}

/// Where one operand's negate and absolute-value modifiers live, or zero when that
/// operand has none.
///
/// gm107.c spells these per opcode rather than per operand position: `fmul` negates
/// only its second source, `ffma` never absolutes anything, and bit 49 is an
/// *absolute* to `fadd` and a *negate* to `ffma`. A zero is safe to return for "no
/// such modifier" because none lives at bit zero -- that belongs to the destination
/// -- so nothing is ever read from it.
struct ModifierBits {
    std::uint32_t negate = 0;
    std::uint32_t absolute = 0;
};

/// `source` is the operand's position in the instruction's own order, matching how
/// the decoder fills `Instr::src`. `iadd3` is deliberately absent: its sources are
/// integers, and a two's-complement negation is not the sign flip the IR's `FNeg`
/// makes, so the decoder refuses those bits instead of lowering them here.
constexpr ModifierBits ModifierBitsOf(ShaderOpcode opcode, std::uint32_t source) {
    if (opcode == ShaderOpcode::FAdd || opcode == ShaderOpcode::Fmnmx) {
        if (source == 0) {
            return {kFormat.arith_negate_a_bit, kFormat.arith_absolute_a_bit};
        }
        if (source == 1) {
            return {kFormat.arith_negate_b_bit, kFormat.arith_absolute_b_bit};
        }
        return {};
    }
    if (opcode == ShaderOpcode::FMul) {
        return source == 1 ? ModifierBits{kFormat.fmul_negate_b_bit, 0} : ModifierBits{};
    }
    if (opcode == ShaderOpcode::Ffma) {
        if (source == 1) {
            return {kFormat.ffma_negate_b_bit, 0};
        }
        if (source == 2) {
            return {kFormat.ffma_negate_c_bit, 0};
        }
        return {};
    }
    if (opcode == ShaderOpcode::Mufu) {
        return source == 0 ? ModifierBits{kFormat.mufu_negate_bit, kFormat.mufu_absolute_bit}
                           : ModifierBits{};
    }
    if (opcode == ShaderOpcode::F2f) {
        return source == 0
                   ? ModifierBits{kFormat.f2f_source_neg_bit, kFormat.f2f_source_abs_bit}
                   : ModifierBits{};
    }
    return {};
}

}  // namespace

std::uint64_t HashShaderBytes(std::span<const std::uint8_t> bytes) {
    // FNV-1a: a plain structural hash, nothing clever.
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ull;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x0000'0100'0000'01B3ull;
    }
    return hash;
}

std::optional<ShaderBinaryHeader> ParseShaderBinary(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes) {
        return std::nullopt;
    }
    std::uint32_t words[4] = {};
    for (std::size_t index = 0; index < 4; ++index) {
        std::memcpy(&words[index], bytes.data() + index * sizeof(std::uint32_t),
                    sizeof(std::uint32_t));
    }
    if (words[0] != kShaderMagic) {
        return std::nullopt;
    }

    ShaderBinaryHeader header;
    header.version = words[1];
    header.instruction_count = words[2];
    header.register_count = words[3];
    header.code_offset = static_cast<std::uint32_t>(kHeaderBytes);

    // The code must actually be present.
    const std::uint64_t code_bytes =
        static_cast<std::uint64_t>(header.instruction_count) * kInstructionBytes;
    if (header.instruction_count == 0 || code_bytes > bytes.size() - kHeaderBytes) {
        return std::nullopt;
    }
    if (header.register_count == 0) {
        return std::nullopt;
    }
    return header;
}

std::optional<Module> DecodeShader(std::span<const std::uint8_t> bytes, Stage stage,
                                   std::string& error) {
    const auto header = ParseShaderBinary(bytes);
    if (!header.has_value()) {
        error = "the shader binary has no valid header";
        return std::nullopt;
    }

    Module module;
    module.stage = stage;
    module.signature = HashShaderBytes(bytes);
    module.main.stage = stage;
    module.main.predicate_count = 8;
    module.main.gpr_count = static_cast<std::uint8_t>(
        header->register_count > 255 ? 255 : header->register_count);
    module.main.blocks.resize(1);
    std::vector<Instr>& code = module.main.blocks[0].code;

    // Registers the decoder needs for itself: one per operand it has to materialise
    // or modify, because a modified operand has to stay live until the operation that
    // consumes it. Allocated on first use and reported in gpr_count so both consumers
    // size their register file for them.
    std::int32_t next_scratch = -1;

    // The attribute slots the code names. The declaration follows the code rather
    // than the stage, so nothing is invented -- and only the slots actually read
    // or written are declared, which is what lets a fragment shader that reads one
    // varying at location 6 declare exactly one input rather than seven.
    //
    // An instruction's `index` holds a *slot* while decoding and is rewritten to
    // that slot's ordinal in the declared list afterwards, because the IR indexes
    // its input and output vectors by ordinal while the pipeline matches on
    // location.
    std::vector<std::uint32_t> input_slots;
    std::vector<std::uint32_t> output_slots;
    const auto note_slot = [](std::vector<std::uint32_t>& slots, std::uint32_t slot) {
        if (std::find(slots.begin(), slots.end(), slot) == slots.end()) {
            slots.push_back(slot);
            // Ascending, so the declared locations read in order and a slot's
            // ordinal cannot change when later code names a lower slot.
            std::sort(slots.begin(), slots.end());
        }
    };

    for (std::uint32_t index = 0; index < header->instruction_count; ++index) {
        const std::uint8_t* instruction_bytes =
            bytes.data() + kHeaderBytes + index * kInstructionBytes;
        const std::uint64_t word = ReadInstruction(instruction_bytes);

        const OpcodePattern* pattern = MatchOpcode(word);
        if (pattern == nullptr) {
            error = "the shader uses an instruction the decoder does not implement";
            return std::nullopt;
        }

        Guard guard;
        if (pattern->guarded && !ReadGuard(word, guard, error)) {
            return std::nullopt;
        }

        const std::uint32_t dst = Field(word, kFormat.dst_bit, kFormat.reg_bits);
        const std::uint32_t src_a = Field(word, kFormat.src_a_bit, kFormat.reg_bits);
        const std::uint32_t src_b = Field(word, kFormat.src_b_bit, kFormat.reg_bits);
        const std::uint32_t src_c = Field(word, kFormat.src_c_bit, kFormat.reg_bits);

        // Fill a whole register with one scalar. MOV and MOV32I name a register
        // with no write mask in the encoding, and the IR is lane-wise, so the
        // value is written to every component.
        const auto fill_register = [&](std::uint32_t reg, float value) {
            for (std::uint8_t lane = 0; lane < 4; ++lane) {
                Instr load;
                load.op = Op::LoadImm;
                load.dst = Gpr(reg);
                load.dst.component = lane;
                load.imm_f = value;
                ApplyGuard(guard, load);
                code.push_back(load);
            }
        };

        // A register of the decoder's own, allocated on first use. Returns -1 when
        // the shader leaves none free.
        const auto allocate_scratch = [&]() -> std::int32_t {
            if (next_scratch < 0) {
                next_scratch = static_cast<std::int32_t>(header->register_count);
            }
            if (next_scratch >= 255) {
                return -1;
            }
            const std::int32_t allocated = next_scratch++;
            module.main.gpr_count = static_cast<std::uint8_t>(next_scratch);
            return allocated;
        };

        // Lowers one operand's modifiers. Neither consumer of the IR reads
        // `Instr::negate` or `Instr::abs`, so a modifier becomes an instruction of its
        // own on a register the shader does not use -- and both are exact (a negation
        // flips a sign, an absolute clears one), so the value is the one the hardware's
        // modifier would have produced. `source` is the operand's position in this
        // instruction's order, which is how `ModifierBitsOf` names them.
        const auto modify = [&](std::uint32_t reg, std::uint32_t source) -> std::int32_t {
            const ModifierBits bits = ModifierBitsOf(pattern->opcode, source);
            const bool negate = bits.negate != 0 && Field(word, bits.negate, 1) != 0;
            const bool absolute = bits.absolute != 0 && Field(word, bits.absolute, 1) != 0;
            if (!negate && !absolute) {
                return static_cast<std::int32_t>(reg);
            }
            if (negate && absolute) {
                // The table prints the two flags in the order "neg, abs", which reads
                // as a negation of a magnitude -- but nothing needs the combination and
                // no source states the reading, so it is refused rather than guessed.
                error = "an operand with both a negate and an absolute is not implemented";
                return -1;
            }
            const std::int32_t target = allocate_scratch();
            if (target < 0) {
                error = "the shader declares more registers than the decoder can lower";
                return -1;
            }
            Instr instruction;
            instruction.op = absolute ? Op::FAbs : Op::FNeg;
            instruction.dst = Gpr(static_cast<std::uint32_t>(target));
            instruction.src[0] = Gpr(reg);
            instruction.src_count = 1;
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            return target;
        };

        // The constant-bank operand of an ALU instruction.
        //
        // The hardware broadcasts the *one* thirty-two-bit word at that offset to
        // every lane, and the IR has no op that loads a single scalar from a buffer,
        // so it is lowered as the sixteen-byte vector the offset sits inside and then
        // a splat of the lane that word is. Both steps are exact, so the value is the
        // one the instruction means. A round trip cannot check this: the encoder wrote
        // the operand and the decoder read the whole vector, and the two directions
        // agreed on something the hardware does not do.
        const auto materialise = [&](std::uint32_t bank, std::uint32_t byte_offset) -> std::int32_t {
            const std::int32_t reg = allocate_scratch();
            if (reg < 0) {
                return -1;
            }
            Instr load;
            load.op = Op::LoadConst;
            load.dst = Gpr(static_cast<std::uint32_t>(reg));
            load.index = bank;
            load.imm_u = byte_offset & ~15u;
            ApplyGuard(guard, load);
            code.push_back(load);

            Instr splat;
            splat.op = Op::Splat;
            splat.dst = Gpr(static_cast<std::uint32_t>(reg));
            splat.src[0] = Gpr(static_cast<std::uint32_t>(reg));
            splat.src[0].component = static_cast<std::uint8_t>((byte_offset & 15u) / 4u);
            splat.src_count = 1;
            ApplyGuard(guard, splat);
            code.push_back(splat);
            return reg;
        };

        const auto bank_operand = [&](std::uint32_t bank_bit) {
            return Field(word, bank_bit, kFormat.const_bank_bits);
        };
        const auto offset_operand = [&]() {
            return static_cast<std::uint32_t>(
                SignExtend(Field(word, kFormat.const_offset_bit, kFormat.const_offset_bits),
                           kFormat.const_offset_bits)
                << kFormat.const_offset_shift);
        };

        switch (pattern->opcode) {
        case ShaderOpcode::Exit: {
            Instr instruction;
            instruction.op = Op::Return;
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }

        case ShaderOpcode::Mov32I:
            // The 32-bit immediate is a float bit pattern in practice
            // (0x3f800000 is 1.0f), which is the usual use of MOV32I.
            fill_register(dst, FloatOf(Field(word, kFormat.imm32_bit, 32)));
            break;

        case ShaderOpcode::Mov:
            if (pattern->source == SourceKind::Register) {
                Instr instruction;
                instruction.op = Op::Mov;
                instruction.dst = Gpr(dst);
                instruction.src[0] = Gpr(src_b);
                instruction.src_count = 1;
                ApplyGuard(guard, instruction);
                code.push_back(instruction);
            } else if (pattern->source == SourceKind::Constant) {
                Instr instruction;
                instruction.op = Op::LoadConst;
                instruction.dst = Gpr(dst);
                instruction.index = bank_operand(kFormat.const_bank_bit);
                instruction.imm_u = offset_operand();
                ApplyGuard(guard, instruction);
                code.push_back(instruction);
            } else {
                // S20_2 is a signed integer, and the IR's LoadImm is
                // float-valued, so the value is taken numerically.
                fill_register(dst, static_cast<float>(static_cast<std::int32_t>(ShortImmediate(word))));
            }
            break;

        case ShaderOpcode::FAdd:
        case ShaderOpcode::FMul:
        case ShaderOpcode::Fmnmx:
        case ShaderOpcode::IAdd3: {
            if (pattern->opcode == ShaderOpcode::IAdd3) {
                // iadd3 is the one arithmetic instruction with fields the decoder
                // does not model: a third source (the IR's IAdd is binary), the mode
                // that changes what is added, the condition code it can consume, and
                // negated sources -- which are two's-complement negations rather than
                // the sign flips the IR's FNeg makes. Any of them present is refused
                // rather than dropped.
                const bool negated = Field(word, kFormat.iadd3_negate_a_bit, 1) != 0 ||
                                     Field(word, kFormat.iadd3_negate_b_bit, 1) != 0 ||
                                     Field(word, kFormat.iadd3_negate_c_bit, 1) != 0;
                if (src_c != kFormat.zero_register ||
                    Field(word, kFormat.iadd3_mode_bit, kFormat.iadd3_mode_bits) != 0 ||
                    Field(word, kFormat.iadd3_condition_code_bit, 1) != 0 || negated) {
                    error = "iadd3 with a third source, a mode, a condition code or a negated "
                            "source is not implemented";
                    return std::nullopt;
                }
            }

            // fmnmx is the same two sources over the same three operand forms as
            // fadd and fmul; what differs is that its predicate operand chooses
            // between the two results.
            Op lowered = Op::IAdd;
            if (pattern->opcode == ShaderOpcode::FAdd) {
                lowered = Op::FAdd;
            } else if (pattern->opcode == ShaderOpcode::FMul) {
                lowered = Op::FMul;
            } else if (pattern->opcode == ShaderOpcode::Fmnmx) {
                const std::uint32_t predicate =
                    Field(word, kFormat.fmnmx_predicate_bit, kFormat.fmnmx_predicate_bits);
                if (predicate != kFormat.fmnmx_predicate_true) {
                    // A predicate register's value is not in the instruction
                    // stream, so which of the two this computes cannot be known.
                    error = "fmnmx whose selector is a predicate register is not implemented";
                    return std::nullopt;
                }
                // Which polarity means *minimum* is in neither the table nor the
                // prose reference: this assignment is a convention, and the
                // encoder mirrors it, so the two directions cannot disagree about
                // a shader the engine wrote itself.
                lowered = Field(word, kFormat.fmnmx_predicate_not_bit, 1) == 0 ? Op::FMin
                                                                               : Op::FMax;
            }

            const std::int32_t first = modify(src_a, 0);
            if (first < 0) {
                return std::nullopt;
            }
            Instr instruction;
            instruction.op = lowered;
            instruction.dst = Gpr(dst);
            instruction.src[0] = Gpr(static_cast<std::uint32_t>(first));
            instruction.src_count = 2;

            switch (pattern->source) {
            case SourceKind::Register: {
                const std::int32_t second = modify(src_b, 1);
                if (second < 0) {
                    return std::nullopt;
                }
                instruction.src[1] = Gpr(static_cast<std::uint32_t>(second));
                break;
            }
            case SourceKind::Constant: {
                const std::int32_t reg = materialise(bank_operand(kFormat.const_bank_bit),
                                                     offset_operand());
                if (reg < 0) {
                    error = "the shader declares more registers than the decoder can lower";
                    return std::nullopt;
                }
                const std::int32_t second = modify(static_cast<std::uint32_t>(reg), 1);
                if (second < 0) {
                    return std::nullopt;
                }
                instruction.src[1] = Gpr(static_cast<std::uint32_t>(second));
                break;
            }
            case SourceKind::Immediate: {
                const std::int32_t reg = allocate_scratch();
                if (reg < 0) {
                    error = "the shader declares more registers than the decoder can lower";
                    return std::nullopt;
                }
                // F20_2 for the float ops, S20_2 for the integer add.
                const auto value = pattern->opcode == ShaderOpcode::IAdd3
                                       ? static_cast<float>(static_cast<std::int32_t>(
                                             ShortImmediate(word)))
                                       : FloatImmediate(word);
                fill_register(static_cast<std::uint32_t>(reg), value);
                const std::int32_t second = modify(static_cast<std::uint32_t>(reg), 1);
                if (second < 0) {
                    return std::nullopt;
                }
                instruction.src[1] = Gpr(static_cast<std::uint32_t>(second));
                break;
            }
            default:
                error = "internal: unexpected operand form";
                return std::nullopt;
            }

            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }

        case ShaderOpcode::Ffma: {
            // ffma dst, a, b, c -- three sources, so unlike the binary ops it
            // reads src_c as well. The second source has all three forms; the
            // third is always a register.
            const std::int32_t first = modify(src_a, 0);
            if (first < 0) {
                return std::nullopt;
            }
            Instr instruction;
            instruction.op = Op::FFma;
            instruction.dst = Gpr(dst);
            instruction.src[0] = Gpr(static_cast<std::uint32_t>(first));
            instruction.src_count = 3;

            switch (pattern->source) {
            case SourceKind::Register: {
                const std::int32_t second = modify(src_b, 1);
                if (second < 0) {
                    return std::nullopt;
                }
                instruction.src[1] = Gpr(static_cast<std::uint32_t>(second));
                break;
            }
            case SourceKind::Constant: {
                const std::int32_t reg = materialise(bank_operand(kFormat.const_bank_bit),
                                                     offset_operand());
                if (reg < 0) {
                    error = "the shader declares more registers than the decoder can lower";
                    return std::nullopt;
                }
                const std::int32_t second = modify(static_cast<std::uint32_t>(reg), 1);
                if (second < 0) {
                    return std::nullopt;
                }
                instruction.src[1] = Gpr(static_cast<std::uint32_t>(second));
                break;
            }
            case SourceKind::Immediate: {
                const std::int32_t reg = allocate_scratch();
                if (reg < 0) {
                    error = "the shader declares more registers than the decoder can lower";
                    return std::nullopt;
                }
                fill_register(static_cast<std::uint32_t>(reg), FloatImmediate(word));
                const std::int32_t second = modify(static_cast<std::uint32_t>(reg), 1);
                if (second < 0) {
                    return std::nullopt;
                }
                instruction.src[1] = Gpr(static_cast<std::uint32_t>(second));
                break;
            }
            default:
                error = "internal: unexpected operand form";
                return std::nullopt;
            }

            const std::int32_t third = modify(src_c, 2);
            if (third < 0) {
                return std::nullopt;
            }
            instruction.src[2] = Gpr(static_cast<std::uint32_t>(third));
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }

        case ShaderOpcode::F2f: {
            // A conversion carrying a rounding mode. The two type fields sit in the
            // register field REG_08 -- the table reads bits 8..9 as the source type
            // and bits 10..11 as the destination -- so this instruction's one source
            // is REG_20 and there is no src_a to read.
            const std::uint32_t source_type =
                Field(word, kFormat.f2f_source_type_bit, kFormat.f2f_type_bits);
            const std::uint32_t dest_type =
                Field(word, kFormat.f2f_dest_type_bit, kFormat.f2f_type_bits);
            if (source_type != kFormat.f2f_type_f32 || dest_type != kFormat.f2f_type_f32) {
                error = "a conversion that is not float to float is not implemented";
                return std::nullopt;
            }
            // Floor is the only mode the IR has a counterpart for. The enable bit
            // belongs to the same field (tab5ca8_0): clearing it is what that
            // table's nameless first entry and its "pass" spell.
            if (Field(word, kFormat.f2f_rounding_enable_bit, 1) == 0 ||
                Field(word, kFormat.f2f_rounding_bit, kFormat.f2f_rounding_bits) !=
                    kFormat.f2f_rounding_floor) {
                error = "f2f with a rounding mode other than floor is not implemented";
                return std::nullopt;
            }
            // Flushing to zero and saturating are not modelled and would change the
            // value; the source's negate and absolute are, and are lowered below.
            if (Field(word, kFormat.f2f_ftz_bit, 1) != 0 ||
                Field(word, kFormat.f2f_sat_bit, 1) != 0) {
                error = "a conversion with a flush-to-zero or saturating modifier is not "
                        "implemented";
                return std::nullopt;
            }

            const std::int32_t source = modify(src_b, 0);
            if (source < 0) {
                return std::nullopt;
            }
            Instr instruction;
            instruction.op = Op::FFloor;
            instruction.dst = Gpr(dst);
            instruction.src[0] = Gpr(static_cast<std::uint32_t>(source));
            instruction.src_count = 1;
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }

        case ShaderOpcode::Mufu: {
            // One source, one destination, and a four-bit function selector.
            // gm107.c tab5080_0: rcp is 0x4 and sqrt is 0x8 (the latter marked
            // .fmask = F_SM52, which is this chip). Everything else the table lists
            // -- cos, ex2, lg2, rsq -- is refused rather than approximated.
            const std::uint32_t function =
                Field(word, kFormat.mufu_select_bit, kFormat.mufu_select_bits);
            Op lowered = Op::Nop;
            if (function == kFormat.mufu_rcp) {
                lowered = Op::FRcp;
            } else if (function == kFormat.mufu_sqrt) {
                lowered = Op::FSqrt;
            } else {
                error = "mufu function 0x" + std::to_string(function) + " is not implemented";
                return std::nullopt;
            }

            const std::int32_t source = modify(src_a, 0);
            if (source < 0) {
                return std::nullopt;
            }
            Instr instruction;
            instruction.op = lowered;
            instruction.dst = Gpr(dst);
            instruction.src[0] = Gpr(static_cast<std::uint32_t>(source));
            instruction.src_count = 1;
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }

        case ShaderOpcode::Fswzadd: {
            // The one instruction in this set that moves a value across lanes,
            // which is what the IR's Splat is. `fswzadd dst, a, RZ, swizzle` with
            // every output lane naming the same source component is exactly a
            // broadcast; a second source would make it an add as well, and a
            // swizzle that is not uniform is a genuine shuffle, which the IR has
            // no op for. Both are refused rather than approximated.
            if (Field(word, kFormat.fswzadd_ndv_bit, 1) != 0) {
                error = "fswzadd with its ndv flag set is not implemented";
                return std::nullopt;
            }
            if (src_b != kFormat.zero_register) {
                error = "fswzadd with a second source operand is not implemented";
                return std::nullopt;
            }

            constexpr std::uint32_t kComponentMask = 0x3;
            static_assert(kComponentMask == (1u << kFormat.fswzadd_component_bits) - 1u,
                          "the swizzle's component fields are two bits wide");
            const std::uint32_t swizzle =
                Field(word, kFormat.fswzadd_swizzle_bit, kFormat.fswzadd_swizzle_bits);
            const std::uint32_t component = swizzle & kComponentMask;
            for (std::uint32_t lane = 1; lane < kFormat.fswzadd_lanes; ++lane) {
                if (((swizzle >> (lane * kFormat.fswzadd_component_bits)) & kComponentMask) !=
                    component) {
                    error = "fswzadd with a non-uniform swizzle is not implemented";
                    return std::nullopt;
                }
            }

            Instr instruction;
            instruction.op = Op::Splat;
            instruction.dst = Gpr(dst);
            instruction.src[0] = Value{RegKind::Gpr, static_cast<std::uint8_t>(src_a),
                                       static_cast<std::uint8_t>(component)};
            instruction.src_count = 1;
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }

        case ShaderOpcode::Ld:
            if (src_a != kFormat.zero_register) {
                error = "a register-indexed load is not implemented";
                return std::nullopt;
            }
            {
                Instr instruction;
                instruction.dst = Gpr(dst);
                if (pattern->source == SourceKind::Constant) {
                    // ld r, c[bank][ofs]: a constant buffer read. It is modelled as a
                    // sixteen-byte vector, and the table's widest constant word is
                    // eight, so anything but the thirty-two-bit code the encoder writes
                    // would be read as wider than it asked for.
                    if (Field(word, kFormat.ld_size_bit, kFormat.ld_size_bits) !=
                        kFormat.ld_size_b32) {
                        error = "a constant load that is not thirty-two bits wide is not implemented";
                        return std::nullopt;
                    }
                    instruction.op = Op::LoadConst;
                    instruction.index = bank_operand(kFormat.ld_bank_bit);
                    instruction.imm_u = static_cast<std::uint32_t>(
                        SignExtend(Field(word, kFormat.ld_offset_bit, kFormat.ld_offset_bits),
                                   kFormat.ld_offset_bits));
                } else {
                    // ld r, a[ofs]: attribute memory, where a shader's per-vertex inputs
                    // live. Four components is one IR slot, so only the sixteen-byte
                    // access is modelled -- a narrow read is not "the first component".
                    if (Field(word, kFormat.attr_size_bit, kFormat.attr_size_bits) !=
                        kFormat.attr_size_b128) {
                        error = "an attribute load that is not sixteen bytes wide is not implemented";
                        return std::nullopt;
                    }
                    instruction.op = Op::LoadAttribute;
                    instruction.index =
                        Field(word, kFormat.attr_offset_bit, kFormat.attr_offset_bits) / kIoSlotBytes;
                    note_slot(input_slots, instruction.index);
                }
                ApplyGuard(guard, instruction);
                code.push_back(instruction);
            }
            break;

        case ShaderOpcode::St:
            if (src_a != kFormat.zero_register) {
                error = "a register-indexed store is not implemented";
                return std::nullopt;
            }
            if (Field(word, kFormat.attr_size_bit, kFormat.attr_size_bits) !=
                kFormat.attr_size_b128) {
                error = "an attribute store that is not sixteen bytes wide is not implemented";
                return std::nullopt;
            }
            {
                Instr instruction;
                instruction.op = Op::StoreOutput;
                // For a store the REG_00 field holds the data being written.
                instruction.src[0] = Gpr(dst);
                instruction.src_count = 1;
                instruction.index =
                    Field(word, kFormat.attr_offset_bit, kFormat.attr_offset_bits) / kIoSlotBytes;
                note_slot(output_slots, instruction.index);
                ApplyGuard(guard, instruction);
                code.push_back(instruction);
            }
            break;

        case ShaderOpcode::Ipa:
            if (src_a != kFormat.zero_register) {
                error = "a register-indexed interpolation is not implemented";
                return std::nullopt;
            }
            {
                Instr instruction;
                instruction.op = Op::Interpolate;
                instruction.dst = Gpr(dst);
                instruction.index =
                    Field(word, kFormat.attr_offset_alt_bit, kFormat.attr_offset_bits) / kIoSlotBytes;
                note_slot(input_slots, instruction.index);
                // The barycentric weight registers and the reference predicate
                // are not lowered: the rasteriser supplies the interpolated
                // value to the interpreter.
                ApplyGuard(guard, instruction);
                code.push_back(instruction);
            }
            break;

        case ShaderOpcode::Tex:
        case ShaderOpcode::Tld: {
            // A texture fetch (sampling) and a texture load (an unfiltered read
            // at integer coordinates). The engine names a texture by the *slot*
            // the draw bound, and the field the encoding offers for that is the
            // index, so the texture reference register is required to be RZ: a
            // descriptor held in a register would have to be mapped back to a
            // slot, and the decoder has no such map to consult.
            if (src_a != kFormat.zero_register) {
                error = "a texture fetch through a register descriptor is not implemented";
                return std::nullopt;
            }
            if (pattern->opcode == ShaderOpcode::Tld &&
                Field(word, kFormat.texture_form_bit, 1) != 0) {
                error = "the form of tld that carries no texture index is not implemented";
                return std::nullopt;
            }
            if (Field(word, kFormat.texture_dim_bit, kFormat.texture_dim_bits) !=
                kFormat.texture_dim_2d) {
                error = "a texture fetch that is not two-dimensional is not implemented";
                return std::nullopt;
            }
            if (Field(word, kFormat.texture_array_bit, 1) != 0) {
                error = "a texture array fetch is not implemented";
                return std::nullopt;
            }
            // Every flag the table names has to be off: one this decoder does not
            // model could change what the fetch returns. The fields the table
            // leaves unnamed are not read at all.
            if (Field(word, kFormat.texture_lod_bit,
                      pattern->opcode == ShaderOpcode::Tex ? kFormat.tex_lod_bits
                                                           : kFormat.tld_lod_bits) != 0) {
                error = "a texture fetch with an explicit level of detail is not implemented";
                return std::nullopt;
            }
            if (Field(word, kFormat.texture_nodep_bit, 1) != 0) {
                error = "a texture fetch that declares no dependency is not implemented";
                return std::nullopt;
            }
            if (pattern->opcode == ShaderOpcode::Tex) {
                if (Field(word, kFormat.tex_aoffi_bit, 1) != 0 ||
                    Field(word, kFormat.tex_dc_bit, 1) != 0) {
                    error = "a texture fetch with a coordinate offset is not implemented";
                    return std::nullopt;
                }
            } else if (Field(word, kFormat.tld_aoffi_bit, 1) != 0 ||
                       Field(word, kFormat.tld_ms_bit, 1) != 0 ||
                       Field(word, kFormat.tld_cl_bit, 1) != 0) {
                error = "a texture load with a coordinate offset or a multisample flag is not "
                        "implemented";
                return std::nullopt;
            }

            Instr instruction;
            instruction.op =
                pattern->opcode == ShaderOpcode::Tex ? Op::SampleTexture : Op::LoadTexture;
            instruction.dst = Gpr(dst);
            instruction.src[0] = Gpr(src_b);
            instruction.src_count = 1;
            instruction.index = Field(word, kFormat.texture_index_bit, kFormat.texture_index_bits);
            ApplyGuard(guard, instruction);
            code.push_back(instruction);
            break;
        }
        }
    }

    // An attribute reference is a slot in the encoding and an ordinal in the IR,
    // so the two ends are joined up here, once the whole shader has been read.
    const auto ordinal_of = [](const std::vector<std::uint32_t>& slots, std::uint32_t slot) {
        return static_cast<std::uint32_t>(std::find(slots.begin(), slots.end(), slot) -
                                          slots.begin());
    };
    for (Instr& instruction : code) {
        switch (instruction.op) {
        case Op::LoadAttribute:
        case Op::Interpolate:
            instruction.index = ordinal_of(input_slots, instruction.index);
            break;
        case Op::StoreOutput:
            instruction.index = ordinal_of(output_slots, instruction.index);
            break;
        default:
            break;
        }
    }

    // Declare exactly the slots the code references, so the pipeline can match
    // them by location.
    for (const std::uint32_t slot : input_slots) {
        module.inputs.push_back({slot, 4, ScalarType::F32, "input" + std::to_string(slot)});
    }
    for (const std::uint32_t slot : output_slots) {
        module.outputs.push_back({slot, 4, ScalarType::F32, "output" + std::to_string(slot)});
    }

    error.clear();
    return module;
}

const Module* ShaderCache::Get(std::span<const std::uint8_t> bytes, Stage stage,
                               std::string& error) {
    const std::uint64_t key = HashShaderBytes(bytes);
    const auto existing = modules_.find(key);
    if (existing != modules_.end()) {
        return &existing->second;
    }

    auto module = DecodeShader(bytes, stage, error);
    if (!module.has_value()) {
        return nullptr;
    }
    const auto [position, inserted] = modules_.emplace(key, std::move(*module));
    (void)inserted;
    return &position->second;
}

}  // namespace zlong::gpu::shader
