#include "zlong/audio/wav_sink.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace zlong::audio {

namespace {

constexpr std::uint32_t kRiffHeaderBytes = 44;
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kBitsPerSample = 16;

void PushLe32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void PushLe16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

/// The forty-four bytes a canonical PCM WAV opens with. The two length fields are
/// filled in from what was written, so this is built at the end and seeked back to.
void PushHeader(std::vector<std::uint8_t>& bytes, std::uint32_t rate,
                std::uint64_t frames_written) {
    const auto data_bytes = static_cast<std::uint32_t>(frames_written * kChannels *
                                                       (kBitsPerSample / 8));
    const auto block_align = static_cast<std::uint16_t>(kChannels * (kBitsPerSample / 8));

    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    PushLe32(bytes, kRiffHeaderBytes - 8 + data_bytes);  // everything after this field
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    PushLe32(bytes, 16);  // the PCM format block's own size
    PushLe16(bytes, kFormatPcm);
    PushLe16(bytes, static_cast<std::uint16_t>(kChannels));
    PushLe32(bytes, rate);
    PushLe32(bytes, rate * block_align);  // bytes per second
    PushLe16(bytes, block_align);
    PushLe16(bytes, kBitsPerSample);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    PushLe32(bytes, data_bytes);
}

}  // namespace

WavSink::WavSink(std::string path, std::uint32_t rate) : path_(std::move(path)), rate_(rate) {
    if (rate_ == 0) {
        error_ = "a WAV file needs a sample rate";
        failed_ = true;
        return;
    }
#if defined(_MSC_VER)
    if (fopen_s(&file_, path_.c_str(), "wb") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(path_.c_str(), "wb");
#endif
    if (file_ == nullptr) {
        error_ = "could not open " + path_ + " for writing";
        failed_ = true;
        return;
    }
    // A header has to come first so the file is playable even if the writer stops; the
    // lengths in it are placeholders until Finish.
    if (!WriteHeader()) {
        return;
    }
}

WavSink::~WavSink() {
    if (file_ != nullptr) {
        // The header's lengths are only right once something has closed the file.
        Finish();
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool WavSink::WriteHeader() {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kRiffHeaderBytes);
    PushHeader(bytes, rate_, frames_written_);
    if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
        error_ = "could not write the WAV header to " + path_;
        failed_ = true;
        return false;
    }
    return true;
}

bool WavSink::Submit(std::span<const Frame> frames) {
    if (failed_ || finished_ || file_ == nullptr) {
        return false;
    }
    if (frames.empty()) {
        return true;
    }

    std::vector<std::int16_t> pcm;
    pcm.reserve(frames.size() * kChannels);
    for (const Frame& frame : frames) {
        for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            pcm.push_back(ToPcm16(frame[channel]));
        }
    }
    const std::size_t bytes = pcm.size() * sizeof(std::int16_t);
    if (std::fwrite(pcm.data(), 1, bytes, file_) != bytes) {
        error_ = "could not write to " + path_;
        failed_ = true;
        return false;
    }
    frames_written_ += frames.size();
    return true;
}

bool WavSink::Finish() {
    if (failed_ || file_ == nullptr) {
        return false;
    }
    if (finished_) {
        return true;
    }
    if (std::fseek(file_, 0, SEEK_SET) != 0 || !WriteHeader()) {
        return false;
    }
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error_ = "could not seek in " + path_;
        failed_ = true;
        return false;
    }
    finished_ = true;
    return true;
}

}  // namespace zlong::audio
