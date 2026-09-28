// 烛龙 (ZhuLong) - a sink that writes a sixteen-bit PCM WAV file.
//
// The audio path's equivalent of rendering into a bitmap: deterministic, needs no
// device, and something a regression test can compare byte for byte.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "zlong/audio/sink.h"

namespace zlong::audio {

class WavSink final : public Sink {
public:
    /// Opens `path` for writing and lays down a header it will fill in at the end. A
    /// failure is reported through `ok()` rather than thrown, the way the backends
    /// report `Initialize`.
    explicit WavSink(std::string path, std::uint32_t rate = kSampleRate);
    ~WavSink() override;

    WavSink(const WavSink&) = delete;
    WavSink& operator=(const WavSink&) = delete;

    bool Submit(std::span<const Frame> frames) override;
    /// Rewrites the header with the lengths now known. Idempotent.
    bool Finish() override;
    const char* name() const noexcept override { return "wav"; }

    bool ok() const noexcept { return file_ != nullptr && !failed_; }
    /// Why it failed, for a diagnostic. Empty while `ok()`.
    const std::string& error() const noexcept { return error_; }
    std::uint64_t frames_written() const noexcept { return frames_written_; }

private:
    bool WriteHeader();

    std::FILE* file_ = nullptr;
    std::string path_;
    std::string error_;
    std::uint32_t rate_ = kSampleRate;
    std::uint64_t frames_written_ = 0;
    bool failed_ = false;
    bool finished_ = false;
};

}  // namespace zlong::audio
