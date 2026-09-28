// 烛龙 (ZhuLong) - a sink that plays into the machine's default output device.
//
// This is platform code, so it lives with the host programs rather than in
// `内核/audio` -- the same reason the viewer's window is here and not in the engine.
// What it implements is the layer's own seam: `audio::Sink`, so the mixer, the engine
// and the guest's audout service all reach a real sound card without knowing it exists.
//
// Three things are deliberately narrower than a general device sink, and refuse rather
// than pretending:
//
//   * the device must already be at 48 kHz stereo, which is what this layer mixes at.
//     A device at another rate would need a resampler, and quietly resampling is a
//     worse lie than saying so.
//   * only the two sample formats WASAPI shared mode actually hands out are taken:
//     32-bit float, and 16-bit integer. Converted, not reinterpreted.
//   * `Submit` copies into a bounded queue and blocks when it is full, so the device
//     -- not the caller -- is the clock. A caller therefore runs ahead by at most one
//     queue's worth and is then paced by what has actually played.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "zlong/audio/format.h"
#include "zlong/audio/sink.h"

namespace zlong::host {

/// The default render device, in shared mode, with an event-driven playback thread.
///
/// Constructing it either opens a device or fails with a reason -- `ok()` says which,
/// like the WAV sink. Nothing throws.
class WasapiSink final : public zlong::audio::Sink {
public:
    WasapiSink();
    ~WasapiSink() override;

    WasapiSink(const WasapiSink&) = delete;
    WasapiSink& operator=(const WasapiSink&) = delete;

    /// Queues `frames`, blocking while the queue is full. False when the device went
    /// away or the sink was never opened.
    bool Submit(std::span<const zlong::audio::Frame> frames) override;

    /// Waits until everything queued has actually played, then stops the device. This
    /// is what makes a program that mixes a fixed number of frames audible to the end
    /// instead of exiting mid-sentence.
    bool Finish() override;

    const char* name() const noexcept override { return "wasapi"; }

    /// Whether a device was opened. False means `error()` says why.
    bool ok() const noexcept { return ok_; }
    const std::string& error() const noexcept { return error_; }

    /// What the device is, for a diagnostic.
    const std::string& device_name() const noexcept { return device_name_; }
    std::uint32_t device_rate() const noexcept { return rate_; }

    // --- what it has done, for a diagnostic and for a host's own check ---------
    /// Frames actually handed to the device, silence included.
    std::uint64_t frames_played() const noexcept;
    /// Frames of silence written because the queue was empty. The first few are the
    /// device starting before the mixer has filled the queue; after that they mean the
    /// caller fell behind, which is worth knowing.
    std::uint64_t silence_frames() const noexcept;
    /// The periods that happened in: counting them separately from the frames says how
    /// often the queue ran dry, not just how much silence that cost.
    std::uint64_t underruns() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string device_name_;
    std::string error_;
    std::uint32_t rate_ = 0;
    bool ok_ = false;
};

/// Open the default output device, or return nullptr with `error` filled in. The
/// factory is what a program should call: it keeps the refusal in one place.
std::unique_ptr<zlong::audio::Sink> OpenDefaultDevice(std::string& error);

}  // namespace zlong::host
