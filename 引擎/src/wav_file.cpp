#include "zlong/engine/wav_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace zlong::engine {

namespace {

constexpr std::uint16_t kFormatPcm = 1;

/// 一个只往前读的小端游标。**每一次读都先问剩下够不够** —— 一段别人给的文件会在任何一个字节上
/// 断掉，而这里所有越界都是"拒绝"而不是"读一段内存"。
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::size_t left() const { return size_ - at_; }
    std::size_t at() const { return at_; }

    bool skip(std::size_t count) {
        if (count > left()) {
            return false;
        }
        at_ += count;
        return true;
    }

    bool u16(std::uint16_t& out) {
        if (left() < 2) {
            return false;
        }
        out = static_cast<std::uint16_t>(data_[at_] | (data_[at_ + 1] << 8));
        at_ += 2;
        return true;
    }

    bool u32(std::uint32_t& out) {
        if (left() < 4) {
            return false;
        }
        out = static_cast<std::uint32_t>(data_[at_]) |
              (static_cast<std::uint32_t>(data_[at_ + 1]) << 8) |
              (static_cast<std::uint32_t>(data_[at_ + 2]) << 16) |
              (static_cast<std::uint32_t>(data_[at_ + 3]) << 24);
        at_ += 4;
        return true;
    }

    /// 读一个四字节的标识（`RIFF` / `fmt ` / `data` …）。
    bool tag(char out[4]) {
        if (left() < 4) {
            return false;
        }
        std::memcpy(out, data_ + at_, 4);
        at_ += 4;
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
};

std::int16_t Sample16(const std::uint8_t* at) {
    const std::uint16_t raw = static_cast<std::uint16_t>(at[0] | (at[1] << 8));
    return static_cast<std::int16_t>(raw);   // C++20：越界的那一半是补码，不是未定义
}

}  // namespace

bool ParseWav(const std::string& bytes, audio::Clip& out, std::string& error) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    Cursor cursor(raw, bytes.size());

    char tag[4];
    std::uint32_t riff_size = 0;
    if (!cursor.tag(tag) || std::memcmp(tag, "RIFF", 4) != 0) {
        error = "不是一个 RIFF 文件";
        return false;
    }
    if (!cursor.u32(riff_size)) {
        error = "RIFF 头就断了";
        return false;
    }
    if (!cursor.tag(tag) || std::memcmp(tag, "WAVE", 4) != 0) {
        error = "是 RIFF，但不是 WAVE";
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data_at = nullptr;
    std::size_t data_bytes = 0;

    // 块一个接一个，认 `fmt ` 和 `data`，其余（`LIST`、`fact`、`bext`…）跳过。**大小按文件里写的
    // 走，不信别的** —— 一段里多出来的字节是别人的事。
    while (cursor.left() >= 8) {
        char id[4];
        std::uint32_t size = 0;
        if (!cursor.tag(id) || !cursor.u32(size)) {
            break;
        }
        const std::size_t body = cursor.at();
        if (size > cursor.left()) {
            error = "有一个块写的大小超过了文件剩下的字节（文件是截断的）";
            return false;
        }
        if (std::memcmp(id, "fmt ", 4) == 0) {
            Cursor field(raw + body, size);
            std::uint32_t byte_rate = 0;
            std::uint16_t block_align = 0;
            if (!field.u16(format) || !field.u16(channels) || !field.u32(rate) ||
                !field.u32(byte_rate) || !field.u16(block_align) || !field.u16(bits)) {
                error = "fmt 块是断的（连十六个字节都没有）";
                return false;
            }
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_at = raw + body;
            data_bytes = size;
            have_data = true;
        }
        if (!cursor.skip(size)) {
            break;
        }
        if ((size & 1u) != 0 && cursor.left() >= 1) {
            cursor.skip(1);   // 块按偶数字节对齐，奇数大小时末尾有一个填充字节
        }
    }

    if (!have_fmt) {
        error = "没有 fmt 块";
        return false;
    }
    if (format != kFormatPcm) {
        error = "不是 PCM 的 WAV（格式 " + std::to_string(format) + "；这一层只认 16 位 PCM）";
        return false;
    }
    if (channels < 1 || channels > 2) {
        error = "有 " + std::to_string(channels) + " 个声道的 WAV（这一层是双声道）";
        return false;
    }
    if (bits != 16) {
        error = "是 " + std::to_string(bits) + " 位的 WAV（这一层只认 16 位）";
        return false;
    }
    if (rate == 0) {
        error = "采样率是 0";
        return false;
    }
    if (!have_data || data_bytes == 0) {
        error = "没有 data 块，或者里面一个采样都没有";
        return false;
    }

    // 一个采样帧 = 每个声道两个字节。末尾那半个帧（大小不是帧的整数倍）丢掉：那是填充，不是样本。
    const std::size_t stride = static_cast<std::size_t>(channels) * 2;
    const std::size_t count = data_bytes / stride;

    audio::Clip clip;
    clip.rate = rate;
    clip.frames.clear();
    clip.frames.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t* frame = data_at + index * stride;
        const std::int16_t left_sample = Sample16(frame);
        // 单声道铺到两边 —— 它本来就该从中间出来，不是从左边。
        const std::int16_t right_sample = channels == 1 ? left_sample : Sample16(frame + 2);
        clip.frames.push_back(audio::Frame{audio::FromPcm16(left_sample),
                                           audio::FromPcm16(right_sample)});
    }

    out = std::move(clip);
    error.clear();
    return true;
}

bool LoadWav(const std::string& path, audio::Clip& out, std::string& error) {
    std::string bytes;
    if (!ReadWholeFile(path, bytes)) {
        error = "读不了 " + path;
        return false;
    }
    if (!ParseWav(bytes, out, error)) {
        error = path + "：" + error;
        return false;
    }
    return true;
}

}  // namespace zlong::engine
