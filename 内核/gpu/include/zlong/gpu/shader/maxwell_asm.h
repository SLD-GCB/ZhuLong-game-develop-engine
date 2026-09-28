// 烛龙 (ZhuLong) - the Maxwell (SM50/SM52) instruction *encoder*.
//
// maxwell.cpp reads a real instruction word and lowers it onto the IR; this is
// the other direction -- a mnemonic and its operands, back to the word. The two
// share one table (shader/maxwell_encoding.h) and the encoder takes each opcode
// base out of it rather than repeating the constant, so a word written here is
// one MatchOpcode lands on by construction.
//
// The engine writes its shaders this way: assemble the words, hand them to
// DecodeShader, and use the module that comes back. Round-tripping is what keeps
// the two directions honest, and it is what the tests check.
//
// Only the forms the decoder implements are encoded. There is no method for
// anything the other direction would refuse, because writing it would produce
// bytes that cannot come back; and a form nothing has a caller for is left out
// rather than widened blind.

#pragma once

#include <cstdint>
#include <vector>

#include "zlong/gpu/shader/maxwell_encoding.h"

namespace zlong::gpu::shader {

/// Which component of a source a broadcast repeats. The numbering is the
/// table's: gm107.c tab5000_3 names selector value 0 "0000", so X is lane 0.
enum class Swizzle : std::uint32_t { X = 0, Y = 1, Z = 2, W = 3 };

/// The two `mufu` functions the decoder lowers, named after the table's selectors.
enum class MufuFunction : std::uint32_t {
    Rcp = kFormat.mufu_rcp,
    Sqrt = kFormat.mufu_sqrt,
};

/// A shader under construction, in Maxwell instruction words.
///
/// Every method appends one 16-byte instruction and returns *this, so a program
/// reads as the listing it is. Attribute operands are named by *slot*, the unit
/// the IR and the renderer use; the byte offset the encoding actually carries is
/// worked out here.
class MaxwellProgram {
public:
    MaxwellProgram& Exit();
    MaxwellProgram& Mov32I(std::uint32_t dst, float value);
    MaxwellProgram& MovReg(std::uint32_t dst, std::uint32_t src);
    MaxwellProgram& MovConst(std::uint32_t dst, std::uint32_t bank, std::uint32_t byte_offset);

    MaxwellProgram& FAddReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FAddConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                              std::uint32_t byte_offset);
    /// dst = a - b, as `fadd dst, a, -b`: Maxwell has no subtract, and the negate
    /// modifier on the second source is what the decoder lowers back to a subtract.
    MaxwellProgram& FSubReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FMulReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FMulConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                              std::uint32_t byte_offset);

    MaxwellProgram& FFmaReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b, std::uint32_t c);
    MaxwellProgram& FFmaConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                              std::uint32_t byte_offset, std::uint32_t c);
    MaxwellProgram& FFmaImm(std::uint32_t dst, std::uint32_t a, float b, std::uint32_t c);

    /// fmnmx with the constant predicate both directions agree on. Which polarity
    /// means minimum is not in the encoding table, so it is a convention rather
    /// than a decoded fact -- see the encoding header and the decoder, which state
    /// the same one. Only the register form is assembled, because only the
    /// register form has a caller.
    MaxwellProgram& FMinReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FMaxReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);

    /// dst = floor(src), assembled as f2f with floor rounding: this family has no
    /// floor opcode of its own, and the conversion's mode field is what carries it.
    MaxwellProgram& Floor(std::uint32_t dst, std::uint32_t src);

    /// dst = -src and dst = |src|, as `fadd dst, <modified src>, RZ`. A modifier is
    /// what the hardware charges nothing for, and the encoder writes the two the
    /// decoder lowers to `FNeg` and `FAbs`. Only the first source is modified: the
    /// second is the zero register, so it contributes nothing but the addition.
    MaxwellProgram& Negate(std::uint32_t dst, std::uint32_t src);
    MaxwellProgram& Absolute(std::uint32_t dst, std::uint32_t src);

    MaxwellProgram& Mufu(std::uint32_t dst, std::uint32_t src, MufuFunction function);

    MaxwellProgram& LdConst(std::uint32_t dst, std::uint32_t bank, std::uint32_t byte_offset);
    MaxwellProgram& LdAttribute(std::uint32_t dst, std::uint32_t slot);
    MaxwellProgram& StoreAttribute(std::uint32_t slot, std::uint32_t src);
    MaxwellProgram& Ipa(std::uint32_t dst, std::uint32_t slot);

    /// dst = texture `slot` sampled at the normalized coordinates in `coords`
    /// (the same fetch the IR's SampleTexture is), and the same fetch at integer
    /// coordinates with no filtering (the IR's LoadTexture). Both name a
    /// two-dimensional, non-array texture at its base level, which is what the
    /// decoder accepts: the slot rides in the instruction's index field and the
    /// texture reference register is left as RZ.
    MaxwellProgram& Tex2D(std::uint32_t dst, std::uint32_t coords, std::uint32_t slot);
    MaxwellProgram& Tld2D(std::uint32_t dst, std::uint32_t coords, std::uint32_t slot);

    /// dst = all four lanes of src set to one of src's components. This is the
    /// IR's Splat, and in Maxwell it is `fswzadd` with a zero second source: the
    /// only way the implemented set can move a value across lanes.
    MaxwellProgram& Broadcast(std::uint32_t dst, std::uint32_t src, Swizzle component);

    /// The shader binary DecodeShader reads: the header, then the words.
    std::vector<std::uint8_t> Build(std::uint32_t register_count) const;

    std::size_t size() const noexcept { return instructions_.size(); }

private:
    void Push(std::uint64_t word);

    /// The low word of each instruction; the second word is the control half the
    /// decoder does not read, so it is written as zero at Build time.
    std::vector<std::uint64_t> instructions_;
};

}  // namespace zlong::gpu::shader
