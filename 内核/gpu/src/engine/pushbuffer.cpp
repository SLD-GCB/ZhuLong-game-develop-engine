#include "zlong/gpu/engine/pushbuffer.h"

#include <cstring>

namespace zlong::gpu::engine {

namespace {

// Header layout (our model):
//   [31:29] opcode, [28:16] count - 1, [15:13] subchannel, [12:2] address in
//   words, [1:0] zero.
constexpr std::uint32_t kOpcodeShift = 29;
constexpr std::uint32_t kOpcodeMask = 0x7u;
constexpr std::uint32_t kCountShift = 16;
constexpr std::uint32_t kCountMask = 0x1FFFu;
constexpr std::uint32_t kSubchannelShift = 13;
constexpr std::uint32_t kSubchannelMask = 0x7u;
constexpr std::uint32_t kAddressShift = 2;
constexpr std::uint32_t kAddressMask = 0x7FFu;

std::size_t WordCount(std::uint64_t size) {
    return static_cast<std::size_t>(size / sizeof(std::uint32_t));
}

}  // namespace

bool PushBufferReader::Walk(std::span<const std::uint32_t> words, const MethodHandler& emit,
                            std::string& error) {
    std::size_t index = 0;

    while (index < words.size()) {
        const std::uint32_t header = words[index];
        ++index;

        Method method;
        const std::uint32_t opcode = (header >> kOpcodeShift) & kOpcodeMask;
        switch (opcode) {
        case 0:
            method.opcode = MethodOpcode::Increment;
            break;
        case 1:
            method.opcode = MethodOpcode::NonIncrement;
            break;
        case 2:
            method.opcode = MethodOpcode::Immediate;
            break;
        default:
            // Unknown opcodes are skipped rather than treated as fatal: larger
            // streams degrade instead of failing outright.
            method.opcode = MethodOpcode::Unknown;
            break;
        }

        const std::uint32_t count = (header >> kCountShift) & kCountMask;
        method.subchannel = static_cast<std::uint8_t>((header >> kSubchannelShift) &
                                                      kSubchannelMask);
        method.address = ((header >> kAddressShift) & kAddressMask) * sizeof(std::uint32_t);

        // An immediate method carries exactly one argument regardless of the
        // count field; the other forms carry count + 1.
        method.argument_count =
            method.opcode == MethodOpcode::Immediate ? 1u : (count + 1u);

        if (method.opcode == MethodOpcode::Unknown) {
            continue;
        }
        if (index + method.argument_count > words.size()) {
            error = "command stream truncated: a method's arguments run past the end";
            return false;
        }

        if (!emit(method, words.data() + index)) {
            return true;  // the engine is done with this stream
        }
        index += method.argument_count;
    }

    error.clear();
    return true;
}

bool PushBufferReader::WalkMemory(const std::uint8_t* bytes, std::uint64_t size,
                                  const MethodHandler& emit, std::string& error) {
    if (bytes == nullptr) {
        error = "command stream is not mapped guest memory";
        return false;
    }
    const std::size_t count = WordCount(size);
    std::vector<std::uint32_t> words(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(&words[i], bytes + i * sizeof(std::uint32_t), sizeof(std::uint32_t));
    }
    return Walk(words, emit, error);
}

}  // namespace zlong::gpu::engine
