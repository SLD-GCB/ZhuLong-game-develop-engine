#include "zlong/gpu/shader/maxwell_asm.h"

#include <cstring>

#include "zlong/gpu/shader/maxwell.h"

namespace zlong::gpu::shader {

namespace {

/// One IR IoSlot is four F32 components, so an attribute slot is 16 bytes and the
/// encoded offset counts those -- the arithmetic the decoder's kIoSlotBytes
/// division reverses.
constexpr std::uint32_t kIoSlotBytes = 16;

constexpr std::uint64_t Bits(std::uint64_t value, std::uint32_t bit, std::uint32_t width) {
    return (value & ((1ull << width) - 1ull)) << bit;
}

/// tabpred's "no guard": predicate 7, not negated. An unpredicated instruction
/// carries this in bits 16..19, not zero.
constexpr std::uint64_t BareGuard() {
    return Bits(kFormat.guard_bare, kFormat.guard_bit, kFormat.guard_bits);
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// gm107.c s2020_bf and f1920_bf: a 20-bit value whose top bit is bit 56 rather
/// than bit 39 -- which is why that opcode mask frees bit 56.
constexpr std::uint64_t Split20(std::uint32_t value) {
    return Bits(value, kFormat.imm20_bit, kFormat.imm20_low_bits) |
           Bits(value >> kFormat.imm20_low_bits, kFormat.imm20_high_bit, 1);
}

/// F20_2: the field holds the top 20 bits of the float.
std::uint64_t Float20(float value) {
    return Split20(FloatBits(value) >> kFormat.float20_shift);
}

/// C34_RZ_O14_20: the byte offset in units of four, in fourteen signed bits.
constexpr std::uint64_t ConstOffset(std::uint32_t byte_offset) {
    return Bits(byte_offset >> kFormat.const_offset_shift, kFormat.const_offset_bit,
                kFormat.const_offset_bits);
}

/// The opcode base of an (opcode, operand form) pair, read out of the table the
/// decoder matches against. Every pair named in this file is in it, so the loop
/// always returns -- a pair that is not would have to be added to the table
/// first, and the round-trip tests would catch a word that decodes to nothing.
std::uint64_t Base(ShaderOpcode opcode, SourceKind source) {
    for (const OpcodePattern& pattern : kOpcodePatterns) {
        if (pattern.opcode == opcode && pattern.source == source) {
            return pattern.value;
        }
    }
    return 0;
}

/// fmnmx's predicate operand: the always-true predicate, negated or not. The
/// polarity convention -- true selects the minimum -- is the decoder's, and this
/// is the only place on the writing side that has to know it.
std::uint64_t MinMaxWord(std::uint32_t dst, std::uint32_t a, std::uint32_t b, bool minimum) {
    std::uint64_t word = Base(ShaderOpcode::Fmnmx, SourceKind::Register) | BareGuard() |
                         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
                         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
                         Bits(b, kFormat.src_b_bit, kFormat.reg_bits) |
                         Bits(kFormat.fmnmx_predicate_true, kFormat.fmnmx_predicate_bit,
                              kFormat.fmnmx_predicate_bits);
    if (!minimum) {
        word |= Bits(1, kFormat.fmnmx_predicate_not_bit, 1);
    }
    return word;
}

/// `fadd dst, <one source with a modifier>, RZ` -- the zero register is what makes
/// the addition a no-op, so what lands in `dst` is the modified operand itself.
std::uint64_t AddModifierWord(std::uint32_t dst, std::uint32_t src, std::uint32_t modifier_bit) {
    return Base(ShaderOpcode::FAdd, SourceKind::Register) | BareGuard() |
           Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
           Bits(src, kFormat.src_a_bit, kFormat.reg_bits) |
           Bits(1, modifier_bit, 1) |
           Bits(kFormat.zero_register, kFormat.src_b_bit, kFormat.reg_bits);
}

}  // namespace

void MaxwellProgram::Push(std::uint64_t word) { instructions_.push_back(word); }

MaxwellProgram& MaxwellProgram::Exit() {
    Push(Base(ShaderOpcode::Exit, SourceKind::Register) | BareGuard());
    return *this;
}

MaxwellProgram& MaxwellProgram::Mov32I(std::uint32_t dst, float value) {
    Push(Base(ShaderOpcode::Mov32I, SourceKind::Immediate) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(FloatBits(value), kFormat.imm32_bit, 32));
    return *this;
}

MaxwellProgram& MaxwellProgram::MovReg(std::uint32_t dst, std::uint32_t src) {
    Push(Base(ShaderOpcode::Mov, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(src, kFormat.src_b_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::MovConst(std::uint32_t dst, std::uint32_t bank,
                                         std::uint32_t byte_offset) {
    Push(Base(ShaderOpcode::Mov, SourceKind::Constant) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(bank, kFormat.const_bank_bit, kFormat.const_bank_bits) | ConstOffset(byte_offset));
    return *this;
}

MaxwellProgram& MaxwellProgram::FAddReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b) {
    Push(Base(ShaderOpcode::FAdd, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(b, kFormat.src_b_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::FAddConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                                          std::uint32_t byte_offset) {
    Push(Base(ShaderOpcode::FAdd, SourceKind::Constant) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(bank, kFormat.const_bank_bit, kFormat.const_bank_bits) | ConstOffset(byte_offset));
    return *this;
}

MaxwellProgram& MaxwellProgram::FSubReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b) {
    Push(Base(ShaderOpcode::FAdd, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(b, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(1, kFormat.arith_negate_b_bit, 1));
    return *this;
}

MaxwellProgram& MaxwellProgram::FMulReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b) {
    Push(Base(ShaderOpcode::FMul, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(b, kFormat.src_b_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::FMulConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                                          std::uint32_t byte_offset) {
    Push(Base(ShaderOpcode::FMul, SourceKind::Constant) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(bank, kFormat.const_bank_bit, kFormat.const_bank_bits) | ConstOffset(byte_offset));
    return *this;
}

MaxwellProgram& MaxwellProgram::FFmaReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b,
                                        std::uint32_t c) {
    Push(Base(ShaderOpcode::Ffma, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(b, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(c, kFormat.src_c_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::FFmaConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                                          std::uint32_t byte_offset, std::uint32_t c) {
    Push(Base(ShaderOpcode::Ffma, SourceKind::Constant) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(bank, kFormat.const_bank_bit, kFormat.const_bank_bits) | ConstOffset(byte_offset) |
         Bits(c, kFormat.src_c_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::FFmaImm(std::uint32_t dst, std::uint32_t a, float b,
                                        std::uint32_t c) {
    Push(Base(ShaderOpcode::Ffma, SourceKind::Immediate) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(a, kFormat.src_a_bit, kFormat.reg_bits) | Float20(b) |
         Bits(c, kFormat.src_c_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::Mufu(std::uint32_t dst, std::uint32_t src,
                                     MufuFunction function) {
    Push(Base(ShaderOpcode::Mufu, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(src, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(static_cast<std::uint32_t>(function), kFormat.mufu_select_bit,
              kFormat.mufu_select_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::LdConst(std::uint32_t dst, std::uint32_t bank,
                                        std::uint32_t byte_offset) {
    // No guard bits: the constant form of `ld` is the one pattern that is not
    // predicated, so 16..19 are not a predicate field there.
    Push(Base(ShaderOpcode::Ld, SourceKind::Constant) |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(kFormat.zero_register, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(bank, kFormat.ld_bank_bit, kFormat.const_bank_bits) |
         Bits(byte_offset, kFormat.ld_offset_bit, kFormat.ld_offset_bits) |
         // The decoder models this as a sixteen-byte vector read and requires the
         // thirty-two-bit code, which is the only one the table offers that the
         // engine's vector loads are not asking to be read wider than.
         Bits(kFormat.ld_size_b32, kFormat.ld_size_bit, kFormat.ld_size_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::LdAttribute(std::uint32_t dst, std::uint32_t slot) {
    Push(Base(ShaderOpcode::Ld, SourceKind::Attribute) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         // The base register is RZ: the decoder refuses an indexed load, and a
         // shader's own inputs are addressed by slot alone.
         Bits(kFormat.zero_register, kFormat.src_a_bit, kFormat.reg_bits) |
         // Four components: an attribute slot is a vector, and b128 is the only
         // access this decoder models.
         Bits(kFormat.attr_size_b128, kFormat.attr_size_bit, kFormat.attr_size_bits) |
         Bits(slot * kIoSlotBytes, kFormat.attr_offset_bit, kFormat.attr_offset_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::StoreAttribute(std::uint32_t slot, std::uint32_t src) {
    // For a store the REG_00 field is the data being written, not a destination.
    Push(Base(ShaderOpcode::St, SourceKind::Attribute) | BareGuard() |
         Bits(src, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(kFormat.zero_register, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(kFormat.attr_size_b128, kFormat.attr_size_bit, kFormat.attr_size_bits) |
         Bits(slot * kIoSlotBytes, kFormat.attr_offset_bit, kFormat.attr_offset_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::Ipa(std::uint32_t dst, std::uint32_t slot) {
    // A real ipa names the barycentric weight registers and a reference
    // predicate; the rasteriser supplies the interpolated value instead, which
    // the decoder documents, so both are encoded as RZ.
    Push(Base(ShaderOpcode::Ipa, SourceKind::Attribute) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(kFormat.zero_register, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(slot * kIoSlotBytes, kFormat.attr_offset_alt_bit, kFormat.attr_offset_bits) |
         Bits(kFormat.zero_register, kFormat.src_c_bit, kFormat.reg_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::Broadcast(std::uint32_t dst, std::uint32_t src,
                                          Swizzle component) {
    // fswzadd with the second source zero: every output lane names the same
    // component of `src`, so the swizzle is four copies of one index. That is
    // the shape the decoder lowers to the IR's Splat; any other swizzle is a
    // genuine shuffle and is refused on the way back.
    std::uint32_t swizzle = 0;
    for (std::uint32_t lane = 0; lane < kFormat.fswzadd_lanes; ++lane) {
        swizzle |=
            static_cast<std::uint32_t>(component) << (lane * kFormat.fswzadd_component_bits);
    }
    Push(Base(ShaderOpcode::Fswzadd, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(src, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(kFormat.zero_register, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(swizzle, kFormat.fswzadd_swizzle_bit, kFormat.fswzadd_swizzle_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::Tex2D(std::uint32_t dst, std::uint32_t coords,
                                      std::uint32_t slot) {
    Push(Base(ShaderOpcode::Tex, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         // The texture reference is RZ and the slot is the index: the shape the
         // decoder accepts, and the only one it can map onto a bound texture.
         Bits(kFormat.zero_register, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(coords, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(kFormat.texture_dim_2d, kFormat.texture_dim_bit, kFormat.texture_dim_bits) |
         Bits(slot, kFormat.texture_index_bit, kFormat.texture_index_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::Tld2D(std::uint32_t dst, std::uint32_t coords,
                                      std::uint32_t slot) {
    // The opcode mask frees bit 56, which is what separates the spelling that
    // carries an index from the one that does not; the base value has it clear.
    Push(Base(ShaderOpcode::Tld, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(kFormat.zero_register, kFormat.src_a_bit, kFormat.reg_bits) |
         Bits(coords, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(kFormat.texture_dim_2d, kFormat.texture_dim_bit, kFormat.texture_dim_bits) |
         Bits(slot, kFormat.texture_index_bit, kFormat.texture_index_bits));
    return *this;
}

MaxwellProgram& MaxwellProgram::FMinReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b) {
    Push(MinMaxWord(dst, a, b, /*minimum=*/true));
    return *this;
}

MaxwellProgram& MaxwellProgram::FMaxReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b) {
    Push(MinMaxWord(dst, a, b, /*minimum=*/false));
    return *this;
}

MaxwellProgram& MaxwellProgram::Floor(std::uint32_t dst, std::uint32_t src) {
    // Both type fields say f32, and the mode says floor with its enable set.
    Push(Base(ShaderOpcode::F2f, SourceKind::Register) | BareGuard() |
         Bits(dst, kFormat.dst_bit, kFormat.reg_bits) |
         Bits(src, kFormat.src_b_bit, kFormat.reg_bits) |
         Bits(kFormat.f2f_type_f32, kFormat.f2f_source_type_bit, kFormat.f2f_type_bits) |
         Bits(kFormat.f2f_type_f32, kFormat.f2f_dest_type_bit, kFormat.f2f_type_bits) |
         Bits(kFormat.f2f_rounding_floor, kFormat.f2f_rounding_bit, kFormat.f2f_rounding_bits) |
         Bits(1, kFormat.f2f_rounding_enable_bit, 1));
    return *this;
}

MaxwellProgram& MaxwellProgram::Negate(std::uint32_t dst, std::uint32_t src) {
    Push(AddModifierWord(dst, src, kFormat.arith_negate_a_bit));
    return *this;
}

MaxwellProgram& MaxwellProgram::Absolute(std::uint32_t dst, std::uint32_t src) {
    Push(AddModifierWord(dst, src, kFormat.arith_absolute_a_bit));
    return *this;
}

std::vector<std::uint8_t> MaxwellProgram::Build(std::uint32_t register_count) const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kShaderHeaderBytes + instructions_.size() * kFormat.instruction_bytes);
    const auto push = [&bytes](std::uint32_t value) {
        std::uint8_t raw[sizeof(value)] = {};
        std::memcpy(raw, &value, sizeof(value));
        bytes.insert(bytes.end(), raw, raw + sizeof(value));
    };
    push(kShaderMagic);
    push(kShaderVersion);
    push(static_cast<std::uint32_t>(instructions_.size()));
    push(register_count);
    for (const std::uint64_t word : instructions_) {
        push(static_cast<std::uint32_t>(word & 0xFFFF'FFFFull));
        push(static_cast<std::uint32_t>(word >> 32));
        // The control/scheduling half, which is not modelled.
        push(0);
        push(0);
    }
    return bytes;
}

}  // namespace zlong::gpu::shader
