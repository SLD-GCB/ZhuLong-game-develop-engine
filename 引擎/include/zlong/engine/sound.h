// 烛龙 (ZhuLong) - the sound renderer: what turns a scene's emitters into frames.
//
// The renderer's other half, and the same split. The renderer walks the scene's nodes
// and turns the drawable ones into draws; this walks the same nodes and turns the
// emitters into voices, then hands a block of frames to the mixer. Both take the
// camera as the thing they are for -- for audio it is literally the ears.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "zlong/audio/format.h"
#include "zlong/audio/mixer.h"
#include "zlong/engine/scene.h"

namespace zlong::engine {

class SoundRenderer {
public:
    /// Checks the scene's emitters and gives each one a voice. Once per scene, like the
    /// renderer's Prepare: the sounds are already in memory, so this is validation and
    /// layout rather than an upload.
    bool Prepare(const Scene& scene, std::string& error);

    /// Mixes `frames.size()` frames and advances every emitter. Called as often as the
    /// caller likes -- the mix is continuous across calls, so a caller can hand it
    /// whatever block size its device asks for without a seam.
    bool Mix(const Scene& scene, std::span<audio::Frame> frames, std::string& error);

    /// How many emitters went into the last mix, silent placeholders included.
    std::size_t playing() const noexcept { return voices_.size(); }

private:
    std::vector<audio::Voice> voices_;
    std::size_t emitter_count_ = 0;
};

}  // namespace zlong::engine
