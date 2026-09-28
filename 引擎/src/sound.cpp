#include "zlong/engine/sound.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace zlong::engine {

namespace {

static_assert(audio::kChannels == 2, "the pan law below is written for two channels");

/// Linear rolloff: full volume inside the reference distance, silence at the maximum,
/// a straight line between. Predictable on purpose -- a caller can work out what moving
/// an emitter will sound like without reading the mixer.
float DistanceGain(const Emitter& emitter, float distance) {
    if (distance <= emitter.reference_distance) {
        return 1.0f;
    }
    if (distance >= emitter.max_distance) {
        return 0.0f;
    }
    const float span = emitter.max_distance - emitter.reference_distance;
    return span <= 0.0f ? 0.0f : (emitter.max_distance - distance) / span;
}

/// Equal-power pan: the two gains are one angle's cosine and sine, so a source sweeping
/// past the listener holds its power instead of dipping in the middle the way a linear
/// crossfade does.
std::array<audio::Sample, audio::kChannels> PanGains(const Vec3& direction, const Vec3& right) {
    const float along = std::clamp(Dot(direction, right), -1.0f, 1.0f);
    const float angle = (along + 1.0f) * 0.25f * kPi;  // 0 hard left, pi/2 hard right
    return {std::cos(angle), std::sin(angle)};
}

/// The listener's right axis. A camera looking at its own eye has no forward
/// direction, and then there is no axis to pan against: everything sits in the middle.
bool RightAxis(const Camera& camera, Vec3& right) {
    const Vec3 forward = camera.target - camera.eye;
    if (std::sqrt(Dot(forward, forward)) < 1.0e-6f) {
        return false;
    }
    right = Normalize(Cross(Normalize(forward), camera.up));
    return std::sqrt(Dot(right, right)) > 1.0e-6f;
}

}  // namespace

bool SoundRenderer::Prepare(const Scene& scene, std::string& error) {
    voices_.clear();
    voices_.reserve(scene.emitters.size());
    emitter_count_ = scene.emitters.size();

    for (std::size_t index = 0; index < scene.emitters.size(); ++index) {
        const Emitter& emitter = scene.emitters[index];
        if (emitter.parent != kNoParent &&
            (emitter.parent < 0 ||
             static_cast<std::size_t>(emitter.parent) >= scene.nodes.size())) {
            error = "a sound emitter names a parent the scene does not have";
            return false;
        }
        if (!(emitter.gain >= 0.0f)) {
            error = "a sound emitter has a negative gain";
            return false;
        }
        if (emitter.max_distance < emitter.reference_distance) {
            error = "a sound emitter's maximum distance is inside its reference distance";
            return false;
        }

        audio::Voice voice;
        if (emitter.sound != kNoSound) {
            if (emitter.sound < 0 ||
                static_cast<std::size_t>(emitter.sound) >= scene.sounds.size()) {
                error = "a sound emitter names a sound the scene does not have";
                return false;
            }
            const audio::Clip& clip = scene.sounds[emitter.sound];
            if (!clip.Valid()) {
                error = "a scene sound has no frames, or no rate to play them at";
                return false;
            }
            // A pointer into the scene's own clip: the scene outlives the voice, and a
            // copy per emitter would be a copy of the whole sound.
            voice.clip = &clip;
            voice.step = audio::StepFor(clip);
            voice.loop = emitter.loop;
        }
        voices_.push_back(voice);
    }

    error.clear();
    return true;
}

bool SoundRenderer::Mix(const Scene& scene, std::span<audio::Frame> frames, std::string& error) {
    if (scene.emitters.size() != emitter_count_) {
        error = "the sound scene changed shape since Prepare: re-import it";
        return false;
    }

    // World matrices, one forward pass, exactly as the renderer does it: a parent
    // always precedes its children in the node list.
    std::vector<Mat4> world(scene.nodes.size());
    for (std::size_t index = 0; index < scene.nodes.size(); ++index) {
        const Node& node = scene.nodes[index];
        world[index] = node.parent == kNoParent ? node.local : world[node.parent] * node.local;
    }

    Vec3 right{1.0f, 0.0f, 0.0f};
    const bool panned = RightAxis(scene.camera, right);
    const auto centre = static_cast<audio::Sample>(std::cos(kPi * 0.25f));

    for (std::size_t index = 0; index < scene.emitters.size(); ++index) {
        audio::Voice& voice = voices_[index];
        if (voice.clip == nullptr) {
            continue;  // a silent placeholder: nothing to place, and it never finishes
        }

        // Where this emitter is now. Its parent is a node, so the node walk above and
        // the emitter's own local are the whole path.
        const Emitter& emitter = scene.emitters[index];
        const Mat4 transform =
            emitter.parent == kNoParent ? emitter.local : world[emitter.parent] * emitter.local;
        const Vec3 position{transform.m[0][3], transform.m[1][3], transform.m[2][3]};

        const Vec3 to_emitter = position - scene.camera.eye;
        const float distance = std::sqrt(Dot(to_emitter, to_emitter));
        const audio::Sample level =
            static_cast<audio::Sample>(emitter.gain * DistanceGain(emitter, distance) *
                                       scene.master_gain);

        std::array<audio::Sample, audio::kChannels> pan{centre, centre};
        if (panned && distance > 1.0e-6f) {
            pan = PanGains(Normalize(to_emitter), right);
        }
        for (std::uint32_t channel = 0; channel < audio::kChannels; ++channel) {
            voice.gain[channel] = level * pan[channel];
        }
    }

    audio::Mix(voices_, frames);
    error.clear();
    return true;
}

}  // namespace zlong::engine
