// 烛龙 (ZhuLong) - the guest command stream ("pushbuffer") decoder.
//
// A pushbuffer is a stream of 32-bit words. A *method* is a header word followed
// by its arguments; the header packs an opcode, an argument count, a subchannel
// and a register address within the engine's method space.
//
// Treat the bit layout below as our model of the format: it is isolated here and
// in pushbuffer.cpp so correcting it is a one-file change.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace zlong::gpu::engine {

enum class MethodOpcode : std::uint8_t {
    /// Arguments are written to consecutive method addresses.
    Increment = 0,
    /// Arguments are written to the same method address repeatedly.
    NonIncrement = 1,
    /// A single argument written to the method address, not counted in the
    /// header's count field.
    Immediate = 2,
    Unknown = 3,
};

struct Method {
    /// Byte address within the engine's method space.
    std::uint32_t address = 0;
    /// Number of argument words following the header.
    std::uint32_t argument_count = 0;
    MethodOpcode opcode = MethodOpcode::Increment;
    std::uint8_t subchannel = 0;
};

/// Decoded method arguments, followed by the parsed header.
using MethodHandler = std::function<bool(const Method&, const std::uint32_t*)>;

class PushBufferReader {
public:
    /// Walk a decoded word stream. Returns false (with `error` set) on a
    /// malformed or truncated stream; `emit` returning false stops the walk
    /// cleanly and reports success, which is how an engine signals "I have
    /// everything I need".
    static bool Walk(std::span<const std::uint32_t> words, const MethodHandler& emit,
                     std::string& error);

    /// Convenience: read the stream out of guest memory first.
    static bool WalkMemory(const std::uint8_t* bytes, std::uint64_t size,
                           const MethodHandler& emit, std::string& error);
};

}  // namespace zlong::gpu::engine
