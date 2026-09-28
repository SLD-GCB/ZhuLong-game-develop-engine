// 烛龙 (ZhuLong) - the Maxwell (SM50/SM52) shader instruction encoding, as data.
//
// ============================================================================
// PROVENANCE -- read before changing any value in this file.
//
//   Transcribed from envytools' machine-readable instruction table:
//       envytools/envydis/gm107.c          (fetched 2026-09-25)
//   That table is what envyas and envydis are generated from, and it is the
//   thing emulators transcribe. The prose ISA reference documents only a
//   subset of behaviour and says so:
//       https://envytools.readthedocs.io/en/latest/hw/graph/maxwell/cuda/isa.html
//     ("This currently is not a complete reference ... where behaviour not
//       obvious from envydis/gm107.c can be documented.")
//
//   The Switch's GPU is GM20B, i.e. SM52. gm107_isa_s wires the "sm" variantset
//   (sm50 -> sm50op, sm52 -> sm50op+sm52op, sm60 -> sm52op+sm60op), so the
//   SM50/SM52 opcodes are the applicable ones.
//
//   Every bit position, opcode value and opcode mask below is quoted from that
//   file. Where envydis has a name for a field (U32_20, S20_2, C34_RZ_O14_20,
//   C36_08_S16_20, AMEM, ...) the envydis name is kept so the transcription can
//   be checked against its source. envydis scans a table linearly and takes the
//   first entry whose (value, mask) matches, so the order of kOpcodePatterns is
//   part of the data, not a style choice.
//
//   There is no single "opcode width": bit 56 doubles as the 20th bit of the
//   split short immediate, which is why mov's mask is 0xfef8... while mov32i's
//   is 0xfff0.... The mask carried by each pattern is therefore authoritative.
//
//   NOT represented here: the control/scheduling half of the instruction (the
//   second 64-bit word: tabsched's three 21-bit slots of stall/yield/barrier
//   fields), the operand size and mode tables (T(ef90sz), T(ef58sz),
//   T(eff0_0), T(a000_0), T(a000_1), T(fbe0_0), ...), the branch-target and
//   constant-bank-index registers, and the ~200 opcodes the decoder does not
//   implement.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace zlong::gpu::shader {

/// How one 128-bit instruction word packs its fields.
struct InstructionFormat {
    /// gm107_isa_s declares wsize = 8, so an instruction is two 64-bit words;
    /// the opcode and its operands live in the low word.
    std::size_t instruction_bytes = 16;
    std::size_t operand_word_bytes = 8;

    /// gm107.c reg00_bf{0,8}, reg08_bf{8,8}, reg20_bf{20,8}, reg28_bf{28,8},
    /// reg39_bf{39,8}. 255 is the zero register (reg_sr[]: {255, 0, SR_ZERO}).
    std::uint32_t reg_bits = 8;
    std::uint32_t dst_bit = 0;        // REG_00 -- dst, or the data of a store
    std::uint32_t src_a_bit = 8;      // REG_08 -- second operand, or a base
    std::uint32_t src_b_bit = 20;     // REG_20 -- third operand
    std::uint32_t src_b_alt_bit = 28; // REG_28
    std::uint32_t src_c_bit = 39;     // REG_39 -- fourth operand
    std::uint32_t zero_register = 255;

    /// Guard predicate, gm107.c tabpred[]: bits 16..19, where 0b0111 is the
    /// bare (always-true) form, 0b1111 is "never", and otherwise bits 16..18
    /// select the predicate with bit 19 as its negation.
    std::uint32_t guard_bit = 16;
    std::uint32_t guard_bits = 4;
    std::uint32_t guard_bare = 0x7;
    std::uint32_t guard_never = 0xF;

    /// U32_20, gm107.c u3220_bf{20,32} -- the MOV32I immediate.
    std::uint32_t imm32_bit = 20;
    /// S20_2, gm107.c s2020_bf{{20,19,56,1}} -- a 20-bit signed value split
    /// across bits 20..38 and bit 56 (which is why that bit is outside the
    /// opcode mask). F20_2 (f1920_bf, .shr = 12) is the top 20 bits of a float.
    std::uint32_t imm20_bit = 20;
    std::uint32_t imm20_low_bits = 19;
    std::uint32_t imm20_high_bit = 56;
    std::uint32_t float20_shift = 12;

    /// C34_RZ_O14_20, gm107.c cmem34rzo1420_m = { "c", &cmem34_idx{34,5}, 0,
    /// &o1420_bf{{20,14} SIGNED .shr = 2} } -- constant bank `bank`, byte
    /// offset `ofs` in units of four bytes.
    std::uint32_t const_bank_bit = 34;
    std::uint32_t const_bank_bits = 5;
    std::uint32_t const_offset_bit = 20;
    std::uint32_t const_offset_bits = 14;
    std::uint32_t const_offset_shift = 2;
    /// The function selector of `mufu` (gm107.c tab5080_0: bits 20..23, four bits).
    /// Only the two the engine's own shaders need are named; every other function
    /// the table lists is refused rather than approximated.
    std::uint32_t mufu_select_bit = 20;
    std::uint32_t mufu_select_bits = 4;
    std::uint32_t mufu_rcp = 0x4;
    std::uint32_t mufu_sqrt = 0x8;

    /// The source swizzle of `fswzadd` (gm107.c `U08_28`, i.e. u0828_bf{28,8}).
    /// Eight bits is four two-bit component indices -- one per output lane -- and
    /// that is the whole reason this instruction matters here: it is the only way
    /// the implemented set can move a value *across* lanes, which the IR's
    /// `Splat` needs and four-lane lane-wise arithmetic cannot express.
    ///
    /// The table gives the field's position and width but not the packing inside
    /// it. Lane 0 in the low bits, component 0 meaning the low lane, is the
    /// reading taken here; gm107.c's own tab5000_3 corroborates the component
    /// numbering by naming selector values 0..3 "0000", "1111", "2222", "3333".
    std::uint32_t fswzadd_swizzle_bit = 28;
    std::uint32_t fswzadd_swizzle_bits = 8;
    std::uint32_t fswzadd_component_bits = 2;
    std::uint32_t fswzadd_lanes = 4;
    /// gm107.c `ON(38, ndv)`. What the flag does is not in the table and not in
    /// the prose ISA reference, so a set bit is refused rather than ignored.
    std::uint32_t fswzadd_ndv_bit = 38;

    /// C36_08_S16_20, gm107.c cmem3608s1620_m = { "c", &cmem36_idx{36,5},
    /// &reg08_r, &s1620_bf{{20,16} SIGNED} } -- the indexed constant load
    /// `ld r, c[bank][base+ofs]`.
    std::uint32_t ld_bank_bit = 36;
    std::uint32_t ld_offset_bit = 20;
    std::uint32_t ld_offset_bits = 16;

    /// The attribute access size, gm107.c tabeff0_0 (bits 47..48, two bits: b32, b64,
    /// b96, b128). It shares bit 48 with the constant load's size field because the two
    /// live on different forms of `ld`. Four components is what an IR slot is, so b128
    /// is the only access this decoder models, for `ld` and `st` alike.
    std::uint32_t attr_size_bit = 47;
    std::uint32_t attr_size_bits = 2;
    std::uint32_t attr_size_b128 = 3;

    /// The `ld` operand size on the *constant* form, gm107.c tabef90sz (bits 48..50,
    /// three bits: u8, s8, u16, s16, b32, b64). The encoder writes b32 and the decoder
    /// requires it: this decoder models a constant load as the IR's sixteen-byte vector
    /// read, and no width in that table is sixteen bytes, so a narrower request would
    /// silently be read as a wider one.
    std::uint32_t ld_size_bit = 48;
    std::uint32_t ld_size_bits = 3;
    std::uint32_t ld_size_b32 = 4;

    /// AMEM, gm107.c amem_m = { "a", 0, &reg08_r, &amem20_imm{20,10} } and the
    /// AMEM28 variant used by ipa ({28,10}). The base register is src_a.
    std::uint32_t attr_offset_bit = 20;
    std::uint32_t attr_offset_alt_bit = 28;
    std::uint32_t attr_offset_bits = 10;

    /// The texture fetch (`tex`, `tld`, `txq`, ...). gm107.c prints these as
    /// `dst, REG_08, REG_20, index, flags..., dimension, U04_31`, and a
    /// comparison with `txq` -- which names a texture and no coordinates and
    /// therefore has only the first of the two registers -- is what says REG_08
    /// holds the texture reference and REG_20 the coordinates.
    ///
    /// `U13_36` (u1336_bf = { 36, 13 }) is the index operand. `T(df60_0)`
    /// (tabdf60_0) is the dimension at bits 29..30; bit 28 is `array`; bit 49 is
    /// `nodep`. The level-of-detail field is tabc038_0's for `tex` (three bits at
    /// 55) and tabdc38_0's for `tld` (one bit at 55), and bit 56 is `tld`'s
    /// index-less form: envydis prints a literal 0 for the index there.
    std::uint32_t texture_index_bit = 36;
    std::uint32_t texture_index_bits = 13;
    std::uint32_t texture_form_bit = 56;
    std::uint32_t texture_dim_bit = 29;
    std::uint32_t texture_dim_bits = 2;
    std::uint32_t texture_dim_2d = 1;  // tabdf60_0's "t2d", the entry after t1d
    std::uint32_t texture_array_bit = 28;
    std::uint32_t texture_lod_bit = 55;
    std::uint32_t texture_nodep_bit = 49;
    /// tabc038_0's level-of-detail field, and tabdc38_0's one-bit equivalent.
    std::uint32_t tex_lod_bits = 3;
    std::uint32_t tld_lod_bits = 1;
    /// `tex`'s ON(54, aoffi) and ON(50, dc); `tld`'s ON(35, aoffi), ON(50, ms)
    /// and ON(54, cl). The flag at bit 54 means different things to the two.
    std::uint32_t tex_aoffi_bit = 54;
    std::uint32_t tex_dc_bit = 50;
    std::uint32_t tld_aoffi_bit = 35;
    std::uint32_t tld_ms_bit = 50;
    std::uint32_t tld_cl_bit = 54;

    /// `fmnmx`'s minimum-or-maximum selector: gm107.c line 2005's T(pred39), a
    /// predicate register at bits 39..41 (pred39_bf = { 39, 3 }) whose negation is
    /// bit 42 (tabpred39 = { ON(42, not), PRED39 }). pred_sr names index 7 "PT",
    /// the always-true predicate, and that is the only value a static reading can
    /// resolve -- any other index is a register whose value is not in the stream.
    ///
    /// Which polarity means *minimum* is in neither the table nor the prose
    /// reference. The convention taken below (and mirrored by the encoder) is that
    /// the always-true predicate selects the minimum. It is a label, not a decoded
    /// fact: see the decoder, which says so where it uses these.
    std::uint32_t fmnmx_predicate_bit = 39;
    std::uint32_t fmnmx_predicate_bits = 3;
    std::uint32_t fmnmx_predicate_not_bit = 42;
    std::uint32_t fmnmx_predicate_true = 7;

    /// `f2f`, the conversion that carries the rounding mode. Its two type fields
    /// live in the register field REG_08 -- gm107.c tab5cb8_0 reads bits 8..9 as
    /// the source type and tab5cb0_0 reads bits 10..11 as the destination -- which
    /// is why it has one source (REG_20) rather than two. tab5ca8_0 holds the
    /// conversion mode at bits 39..40 with an enable at bit 42; tab5cb0_1 spells
    /// the same rounding without the enable, so both agree that floor is 1.
    std::uint32_t f2f_source_type_bit = 8;
    std::uint32_t f2f_dest_type_bit = 10;
    std::uint32_t f2f_type_bits = 2;
    std::uint32_t f2f_type_f32 = 2;  // "f32" in both tables: the high bit of each pair
    std::uint32_t f2f_rounding_bit = 39;
    std::uint32_t f2f_rounding_bits = 2;
    std::uint32_t f2f_rounding_enable_bit = 42;
    std::uint32_t f2f_rounding_floor = 1;
    /// `f2f`'s modifiers, none of which the IR models: ftz(44), sat(50), and the
    /// source's negate(45) and absolute(49).
    std::uint32_t f2f_ftz_bit = 44;
    std::uint32_t f2f_source_neg_bit = 45;
    std::uint32_t f2f_source_abs_bit = 49;
    std::uint32_t f2f_sat_bit = 50;

    /// Operand negate and absolute-value modifiers. gm107.c spells these per
    /// opcode rather than per operand position, so the same bit means different
    /// things to different instructions and each group below is quoted from its own
    /// table entry:
    ///
    ///   fadd 2006/2080/2126 and fmnmx 2005/2079/2125 -- first source negate 48 and
    ///     absolute 46, second source negate 45 and absolute 49;
    ///   fmul 2004/2078/2124 -- only the second source, negate 48, no absolute;
    ///   ffma 2034/2104/2152 -- second source negate 48 and third 49, and no
    ///     absolute at all;
    ///   iadd3 1994/2068/2114 -- the three sources negate at 51, 50 and 49. It is
    ///     the one arithmetic instruction with two more fields the decoder does not
    ///     model: `mode` (tab5cc0_0, rs/ls at bits 37..38) changes what is added, and
    ///     `x` at 48 makes it consume the condition code;
    ///   mufu 2062 -- its single source, negate 48 and absolute 46.
    ///
    /// Bit 49 is an *absolute* to fadd and a *negate* to ffma, which is exactly why
    /// these cannot be one shared set. A zero means that operand has no modifier of
    /// that kind, and nothing is ever read from bit zero -- that belongs to the
    /// destination.
    std::uint32_t arith_negate_a_bit = 48;
    std::uint32_t arith_absolute_a_bit = 46;
    std::uint32_t arith_negate_b_bit = 45;
    std::uint32_t arith_absolute_b_bit = 49;
    std::uint32_t fmul_negate_b_bit = 48;
    std::uint32_t ffma_negate_b_bit = 48;
    std::uint32_t ffma_negate_c_bit = 49;
    std::uint32_t iadd3_negate_a_bit = 51;
    std::uint32_t iadd3_negate_b_bit = 50;
    std::uint32_t iadd3_negate_c_bit = 49;
    std::uint32_t iadd3_mode_bit = 37;
    std::uint32_t iadd3_mode_bits = 2;
    std::uint32_t iadd3_condition_code_bit = 48;
    std::uint32_t mufu_negate_bit = 48;
    std::uint32_t mufu_absolute_bit = 46;
};

inline constexpr InstructionFormat kFormat{};

/// The opcodes this decoder implements. Each is a real SM50/SM52 mnemonic; the
/// comment carries the encoding it is matched by.
enum class ShaderOpcode : std::uint32_t {
    Exit,    // 0xe300...   exit
    Mov,     // 0x5c98/0x4c98/0x3898   mov
    Mov32I,  // 0x0100...   mov32i
    FAdd,    // 0x5c58/0x4c58/0x3858   fadd
    FMul,    // 0x5c68/0x4c68/0x3868   fmul
    Ffma,    // 0x5980/0x4980/0x3280   ffma
    Fmnmx,   // 0x5c60/0x4c60/0x3860   fmnmx
    F2f,     // 0x5ca8...   f2f
    IAdd3,   // 0x5cc0/0x4cc0/0x38c0   iadd3
    Ld,      // 0xef90... (constant) or 0xefd8... (attribute)   ld
    St,      // 0xeff0...   st
    Ipa,     // 0xe000...   ipa
    Mufu,    // 0x5080...   mufu
    Fswzadd, // 0x50f8...   fswzadd
    Tex,     // 0xc038...   tex
    Tld,     // 0xdc38/0xdd38   tld
};

/// Which kind of second operand a pattern carries. It is the form that decides
/// how the instruction is lowered, because the IR distinguishes a register
/// source from a constant or immediate one.
enum class SourceKind : std::uint8_t {
    Register,   // REG_20 / REG_39
    Constant,   // C34_RZ_O14_20 / C36_08_S16_20
    Immediate,  // S20_2 / U32_20
    Attribute,  // AMEM / AMEM28
};

struct OpcodePattern {
    std::uint64_t value;
    std::uint64_t mask;
    ShaderOpcode opcode;
    SourceKind source;
    /// Whether the pattern's table entry begins with T(pred). The constant-load
    /// form of `ld` does not, so bits 16..19 are not a guard there.
    bool guarded;
};

/// Scanned in order; the first match wins, exactly as envydis scans tabroot.
inline constexpr OpcodePattern kOpcodePatterns[] = {
    // tabroot: N("exit"), ON(5, "keeprefcount"), T(f0f8_0)
    {0xE300'0000'0000'0000ull, 0xFFF0'0000'0000'0000ull, ShaderOpcode::Exit,
     SourceKind::Register, true},
    // tabroot: N("mov32i"), REG_00, U32_20, U04_12
    {0x0100'0000'0000'0000ull, 0xFFF0'0000'0000'0000ull, ShaderOpcode::Mov32I,
     SourceKind::Immediate, true},
    // tabroot: N("mufu"), T(5080_0), ON(50, sat), REG_00, ON(48, neg), ON(46, abs),
    // REG_08 -- one source, one destination, and a selector naming the function.
    {0x5080'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Mufu,
     SourceKind::Register, true},
    // N("fswzadd"), ON(44, ftz), T(5cb8_2), ON(38, ndv), ON(47, cc), REG_00, REG_08,
    // REG_20, U08_28 -- two sources and a per-lane swizzle. Its mask is tight
    // while `ffma`'s is not, so which of the two a word belongs to is decided by
    // the opcode bits either way.
    {0x50F8'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Fswzadd,
     SourceKind::Register, true},

    // N("mov"), REG_00, REG_20, U04_39
    {0x5C98'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Mov,
     SourceKind::Register, true},
    // N("fadd"), ..., REG_00, ..., REG_08, ..., REG_20
    {0x5C58'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::FAdd,
     SourceKind::Register, true},
    // N("fmul"), ..., REG_00, REG_08, ON(48,neg), REG_20
    {0x5C68'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::FMul,
     SourceKind::Register, true},
    // N("ffma"), T(5980_0), T(5980_1), ON(50, sat), ON(47, cc), REG_00, REG_08,
    // ON(48, neg), REG_20, ON(49, neg), REG_39 -- three sources. Its mask stops at
    // bit 55 because the ftz/fmz and rounding fields sit at 51..54, just under the
    // opcode; that is also why no other opcode can be mistaken for it.
    {0x5980'0000'0000'0000ull, 0xFF80'0000'0000'0000ull, ShaderOpcode::Ffma,
     SourceKind::Register, true},

    // N("mov"), REG_00, C34_RZ_O14_20, U04_39
    {0x4C98'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Mov,
     SourceKind::Constant, true},
    // N("fadd"), ..., REG_00, ..., REG_08, ..., C34_RZ_O14_20
    {0x4C58'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::FAdd,
     SourceKind::Constant, true},
    // N("fmul"), ..., REG_00, REG_08, ON(48,neg), C34_RZ_O14_20
    {0x4C68'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::FMul,
     SourceKind::Constant, true},
    // N("ffma"), ..., REG_00, REG_08, ON(48,neg), C34_RZ_O14_20, ON(49,neg), REG_39
    // -- the second source is the constant, the third is still a register.
    {0x4980'0000'0000'0000ull, 0xFF80'0000'0000'0000ull, ShaderOpcode::Ffma,
     SourceKind::Constant, true},

    // N("mov"), REG_00, S20_20, U04_39
    {0x3898'0000'0000'0000ull, 0xFEF8'0000'0000'0000ull, ShaderOpcode::Mov,
     SourceKind::Immediate, true},
    // N("fadd"), ..., REG_00, ..., REG_08, ..., F20_20
    {0x3858'0000'0000'0000ull, 0xFEF8'0000'0000'0000ull, ShaderOpcode::FAdd,
     SourceKind::Immediate, true},
    // N("fmul"), ..., REG_00, REG_08, ON(48,neg), F20_20
    {0x3868'0000'0000'0000ull, 0xFEF8'0000'0000'0000ull, ShaderOpcode::FMul,
     SourceKind::Immediate, true},
    // N("ffma"), ..., REG_00, REG_08, ON(48,neg), F20_20, ON(49,neg), REG_39
    {0x3280'0000'0000'0000ull, 0xFE80'0000'0000'0000ull, ShaderOpcode::Ffma,
     SourceKind::Immediate, true},

    // N("fmnmx"), ON(44, ftz), ON(47, cc), REG_00, ON(48, neg), ON(46, abs),
    // REG_08, ON(45, neg), ON(49, abs), REG_20, T(pred39) -- the same two sources
    // as fadd, plus a predicate operand that chooses between the two results.
    {0x5C60'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Fmnmx,
     SourceKind::Register, true},
    {0x4C60'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Fmnmx,
     SourceKind::Constant, true},
    {0x3860'0000'0000'0000ull, 0xFEF8'0000'0000'0000ull, ShaderOpcode::Fmnmx,
     SourceKind::Immediate, true},

    // N("f2f"), ON(44, ftz), T(5cb0_0), T(5cb8_0), T(5ca8_0), ON(50, sat),
    // ON(47, cc), REG_00, ON(45, neg), ON(49, abs), REG_20 -- one source, because
    // the register field REG_08 holds the two types instead of a register. Only the
    // register spelling is listed; the constant and immediate ones would need a
    // materialisation path nothing in the engine has a caller for, so they land on
    // "not implemented" instead of being widened blind.
    {0x5CA8'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::F2f,
     SourceKind::Register, true},

    // N("iadd3"), ..., REG_00, ..., REG_08, ..., REG_20, ..., REG_39
    {0x5CC0'0000'0000'0000ull, 0xFFF0'0000'0000'0000ull, ShaderOpcode::IAdd3,
     SourceKind::Register, true},
    // N("iadd3"), ..., REG_00, ..., REG_08, ..., C34_RZ_O14_20, ..., REG_39
    {0x4CC0'0000'0000'0000ull, 0xFFF0'0000'0000'0000ull, ShaderOpcode::IAdd3,
     SourceKind::Constant, true},
    // N("iadd3"), ..., REG_00, ..., REG_08, ..., S20_20, ..., REG_39
    {0x38C0'0000'0000'0000ull, 0xFEF0'0000'0000'0000ull, ShaderOpcode::IAdd3,
     SourceKind::Immediate, true},

    // N("ld"), T(ef90_0), T(ef90sz), REG_00, C36_08_S16_20  -- no T(pred)
    {0xEF90'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Ld,
     SourceKind::Constant, false},
    // N("ld"), ON(32,o), ON(31,p), T(eff0_0), REG_00, AMEM, REG_39
    {0xEFD8'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::Ld,
     SourceKind::Attribute, true},
    // N("st"), ON(31,p), T(eff0_0), AMEM, REG_00, REG_39
    {0xEFF0'0000'0000'0000ull, 0xFFF8'0000'0000'0000ull, ShaderOpcode::St,
     SourceKind::Attribute, true},

    // N("ipa"), T(e000_0), T(e000_1), ON(51,sat), REG_00, AMEM28, REG_20,
    // REG_39, T(pred47) -- the non-indexed form, whose base register is RZ.
    {0xE000'0000'0000'FF00ull, 0xFF00'0040'0000'FF00ull, ShaderOpcode::Ipa,
     SourceKind::Attribute, true},

    // N("tex"), T(c038_0), ON(54, aoffi), ON(50, dc), ON(49, nodep), REG_00,
    // REG_08, REG_20, U13_36, ON(28, array), T(df60_0), U04_31 -- the indexed
    // form, the one that carries the index operand at all (the 0xdeb8 entry
    // prints a literal 0 there). Its mask stops at bit 54, leaving the
    // level-of-detail field above it free so the decoder can read it and refuse
    // everything but level zero.
    {0xC038'0000'0000'0000ull, 0xFC38'0000'0000'0000ull, ShaderOpcode::Tex,
     SourceKind::Register, true},
    // N("tld"), T(dc38_0), ON(35, aoffi), ON(50, ms), ON(54, cl), ON(49, nodep),
    // REG_00, REG_08, REG_20, U13_36, ON(28, array), T(df60_0), U04_31. The mask
    // frees bit 56, which is what separates this spelling (0xdc38) from the
    // index-less one (0xdd38); the decoder refuses the latter rather than read an
    // index that entry says is not there.
    {0xDC38'0000'0000'0000ull, 0xFE38'0000'0000'0000ull, ShaderOpcode::Tld,
     SourceKind::Register, true},
};

/// The first pattern whose masked bits match, or nullptr. envydis semantics.
inline const OpcodePattern* MatchOpcode(std::uint64_t word) {
    for (const OpcodePattern& pattern : kOpcodePatterns) {
        if ((word & pattern.mask) == pattern.value) {
            return &pattern;
        }
    }
    return nullptr;
}

}  // namespace zlong::gpu::shader
