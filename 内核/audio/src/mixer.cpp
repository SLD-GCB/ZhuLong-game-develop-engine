#include "zlong/audio/mixer.h"

#include <cmath>
#include <cstddef>

namespace zlong::audio {

namespace {

/// One clip frame, interpolated between the two the cursor sits between.
///
/// Linear rather than nearest: a rate conversion that picks the closer frame shifts
/// every partial by up to half a frame's worth of phase, which is audible on anything
/// with a tone in it. The last frame interpolates towards the first when the voice
/// loops -- that is what makes a loop seamless -- and towards itself when it does not.
Frame ReadInterpolated(const Clip& clip, double cursor, bool loop) {
    const std::size_t count = clip.frames.size();
    std::size_t index = cursor > 0.0 ? static_cast<std::size_t>(cursor) : 0;
    if (index >= count) {
        index = count - 1;
    }
    std::size_t next = index + 1;
    if (next >= count) {
        next = loop ? 0 : index;
    }

    const double fraction = cursor - static_cast<double>(index);
    const Frame& first = clip.frames[index];
    const Frame& second = clip.frames[next];
    Frame result{};
    for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
        // In double, then rounded once: the same arithmetic wherever this runs.
        result[channel] = static_cast<Sample>(static_cast<double>(first[channel]) +
                                              (static_cast<double>(second[channel]) -
                                               static_cast<double>(first[channel])) *
                                                  fraction);
    }
    return result;
}

}  // namespace

void Mix(std::span<Voice> voices, std::span<Frame> block) {
    for (Frame& frame : block) {
        Frame sum{};
        for (Voice& voice : voices) {
            if (voice.finished || voice.clip == nullptr || voice.clip->empty() ||
                voice.step <= 0.0) {
                continue;
            }
            const Clip& clip = *voice.clip;
            const Frame sample = ReadInterpolated(clip, voice.cursor, voice.loop);
            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                sum[channel] += sample[channel] * voice.gain[channel];
            }

            voice.cursor += voice.step;
            const double length = static_cast<double>(clip.frames.size());
            if (voice.cursor >= length) {
                if (voice.loop) {
                    // fmod rather than a subtraction, because a step larger than the
                    // clip would otherwise leave the cursor past the end again.
                    voice.cursor = std::fmod(voice.cursor, length);
                } else {
                    // Finished the moment its *next* frame would be past the end, so
                    // the last frame is read once and not twice.
                    voice.finished = true;
                }
            }
        }
        frame = sum;
    }
}

}  // namespace zlong::audio
