// 烛龙 (ZhuLong) - the sample format the audio layer works in.
//
// One vocabulary for the whole layer, the way the GPU layer has one for surfaces:
// what a sample is, what a frame is, and what rate they are at. The render path's
// equivalent is surface/format.h.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zlong::audio {

/// Stereo, which is what the console's usual output is and what the engine's own
/// sounds are authored for. A wider stream would change this one number and the
/// sink that writes it.
inline constexpr std::uint32_t kChannels = 2;

/// What a mix is accumulated in: float, so voices sum without clipping *inside* the
/// sum and a gain of exactly one is exact. Quantising to something a device wants is
/// the sink's business, and it is the only place a mix gets clamped.
using Sample = float;

/// One instant across every channel, in channel order. Frames rather than interleaved
/// samples, because a rate conversion reads one frame at a time.
using Frame = std::array<Sample, kChannels>;

/// The rate everything is mixed at. A clip at another rate is converted on the way
/// in, by the mixer.
inline constexpr std::uint32_t kSampleRate = 48000;

/// PCM at its own rate.
struct Clip {
    std::vector<Frame> frames;
    std::uint32_t rate = kSampleRate;

    bool empty() const noexcept { return frames.empty(); }

    /// Whether it can be played at all: something to play, and a rate to play it at.
    bool Valid() const noexcept { return !frames.empty() && rate > 0; }

    /// How long it lasts, in seconds.
    double Seconds() const noexcept {
        return rate == 0 ? 0.0 : static_cast<double>(frames.size()) / rate;
    }
};

/// Clip frames per output frame: one when the rates match, and less when the clip is
/// slower. A voice reads with this so that a clip lands at the pitch it was authored
/// at rather than at whatever rate it happens to be stored in.
inline double StepFor(const Clip& clip) noexcept {
    return clip.rate == 0 ? 1.0 : static_cast<double>(clip.rate) / kSampleRate;
}

/// One sample as signed sixteen-bit PCM: clamped to [-1, 1] and scaled by 32767, rounded
/// to nearest. Clamping happens here and nowhere earlier -- a sum louder than one keeps
/// its shape until this point -- and it lives beside the format rather than beside one
/// sink because every sink that stores or plays integer PCM needs the same number.
inline std::int16_t ToPcm16(Sample sample) noexcept {
    const double clamped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0));
}

/// The inverse: signed sixteen-bit PCM as the float the mixer carries.
///
/// The scale is the same 32767 that ToPcm16 quantises with, and deliberately the same
/// number, so a sample that arrives from a guest comes back out exactly as it came in.
/// The one value that cannot is -32768, which lands one step above itself. It lives here
/// rather than beside a service because every path a guest's PCM takes in needs it.
inline Sample FromPcm16(std::int16_t value) noexcept {
    return static_cast<Sample>(static_cast<double>(value) / 32767.0);
}

}  // namespace zlong::audio
