// 烛龙 (ZhuLong) - Maxwell shader binary: header, decoder, and cache.
//
// The instruction encoding is real Maxwell SM50/SM52, transcribed from
// envytools' envydis/gm107.c -- see shader/maxwell_encoding.h for the exact
// source and for what is and is not represented. This file is the decoder: it
// matches an instruction's (value, mask) opcode pattern, decodes the fields
// that pattern carries, and lowers them onto the shared IR.
//
// The binary *container* (magic, version, instruction count, register count) is
// ours, not the hardware's: a real shader arrives as raw instruction bytes in
// guest memory, and wiring that up is a separate step.
//
// Two things the decoder deliberately does not do, and says so instead of
// guessing: a register-indexed load/store/interpolation is rejected, and an
// iadd3 whose third source is not RZ is rejected. fswzadd is accepted only in the
// shape the IR can express -- a uniform swizzle over one source -- so a genuine
// shuffle and a second source are rejected the same way. tex and tld are read
// only as a two-dimensional, non-array, base-level fetch of a slot named by the
// instruction's index; a descriptor in a register, another dimension, or any of
// the flags the table names are rejected rather than dropped. fmnmx is read only
// through the constant predicate -- a predicate register's value is not in the
// stream -- and f2f only as a float-to-float conversion with floor rounding. An
// operand's negate and absolute modifiers are lowered to `FNeg` and `FAbs`, since
// neither consumer of the IR reads `Instr::negate` or `Instr::abs`, and the two
// together on one operand are refused. RZ is resolved to a register the shader never
// writes, which therefore reads zero in both consumers.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "zlong/gpu/shader/ir.h"

namespace zlong::gpu::shader {

/// The shader binary's preamble (our model of it).
struct ShaderBinaryHeader {
    std::uint32_t version = 0;
    std::uint32_t instruction_count = 0;
    std::uint32_t register_count = 0;
    std::uint32_t code_offset = 0;
};

inline constexpr std::size_t kShaderHeaderBytes = 16;
inline constexpr std::uint32_t kShaderMagic = 0x4853'4C5A;  // 'ZLSH' little-endian
inline constexpr std::uint32_t kShaderVersion = 1;

std::optional<ShaderBinaryHeader> ParseShaderBinary(std::span<const std::uint8_t> bytes);

/// Structural hash of the bytes, used as the cache identity.
std::uint64_t HashShaderBytes(std::span<const std::uint8_t> bytes);

/// Decode a shader binary into our IR. Returns nullopt and fills `error` when the
/// binary is malformed or uses something the decoder does not implement.
std::optional<Module> DecodeShader(std::span<const std::uint8_t> bytes, Stage stage,
                                   std::string& error);

/// Decode-once cache, keyed by the byte hash so a guest rewrite yields a new
/// entry rather than needing invalidation.
class ShaderCache {
public:
    /// Returns nullptr and fills `error` when the binary cannot be decoded.
    const Module* Get(std::span<const std::uint8_t> bytes, Stage stage, std::string& error);

    std::size_t size() const noexcept { return modules_.size(); }
    void Clear() { modules_.clear(); }

private:
    std::map<std::uint64_t, Module> modules_;
};

}  // namespace zlong::gpu::shader
