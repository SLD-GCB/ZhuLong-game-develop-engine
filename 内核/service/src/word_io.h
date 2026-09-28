// 烛龙 (ZhuLong) - little-endian word packing for service data.
//
// A service's request and response data is a byte vector the guest chose the
// shape of. Every read below is bounds-checked, so a short request is refused
// rather than padded with whatever happens to follow it in memory.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zlong::service {

namespace word_io {

/// Cursor over a request's data.
class Words {
public:
    explicit Words(const std::vector<std::uint8_t>& data) : data_(data) {}

    bool U32(std::uint32_t& out) {
        if (at_ + 4 > data_.size()) {
            return false;
        }
        out = static_cast<std::uint32_t>(data_[at_]) |
              (static_cast<std::uint32_t>(data_[at_ + 1]) << 8) |
              (static_cast<std::uint32_t>(data_[at_ + 2]) << 16) |
              (static_cast<std::uint32_t>(data_[at_ + 3]) << 24);
        at_ += 4;
        return true;
    }

    bool U64(std::uint64_t& out) {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        if (!U32(low) || !U32(high)) {
            return false;
        }
        out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
        return true;
    }

    std::size_t remaining() const noexcept { return data_.size() - at_; }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t at_ = 0;
};

inline void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

inline void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    PutU32(out, static_cast<std::uint32_t>(value & 0xFFFF'FFFFull));
    PutU32(out, static_cast<std::uint32_t>(value >> 32));
}

inline void PutBytes(std::vector<std::uint8_t>& out, const void* bytes, std::size_t length) {
    const auto* first = static_cast<const std::uint8_t*>(bytes);
    out.insert(out.end(), first, first + length);
}

/// The value of `data` up to its first NUL, as text. A path travels this way.
inline std::string Text(const std::vector<std::uint8_t>& data) {
    std::size_t length = 0;
    while (length < data.size() && data[length] != 0) {
        ++length;
    }
    return std::string(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
}

}  // namespace word_io

}  // namespace zlong::service
