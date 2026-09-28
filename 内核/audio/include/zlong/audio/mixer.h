// 烛龙 (ZhuLong) - summing voices into frames.
//
// What the software rasteriser is to the render path: the thing that turns a
// description into the samples themselves. A voice reads a clip at the clip's own
// rate and writes into the mix at the output's, so the rate conversion lives here and
// nothing above has to know about it.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "zlong/audio/format.h"

namespace zlong::audio {

/// One clip being mixed.
struct Voice {
    /// What it plays. A null clip contributes nothing and never finishes, so a caller
    /// that has not decided what a voice is yet can leave one in the list.
    const Clip* clip = nullptr;
    /// Where the next output frame reads from, in clip frames. Not an integer,
    /// because a rate conversion lands between two of them.
    double cursor = 0.0;
    /// Clip frames per output frame -- `StepFor(clip)` for a clip at its own pitch.
    double step = 1.0;
    /// Per channel, so the caller can pan. The engine computes these -- from the
    /// listener, the emitter and the material -- and the mixer only applies them.
    std::array<Sample, kChannels> gain{1.0f, 1.0f};
    /// Whether the clip restarts instead of ending.
    bool loop = false;
    /// Set when a non-looping voice has read its last frame. The caller drops it; the
    /// voice itself contributes nothing more.
    bool finished = false;
};

/// Fills `block` with the sum of `voices` and advances every voice that has not
/// finished. The block is *replaced*, not added to, so the same buffer can be handed
/// to a sink afterwards.
void Mix(std::span<Voice> voices, std::span<Frame> block);

}  // namespace zlong::audio
