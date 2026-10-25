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

/// 一个 voice 放的是"哪一段"的指纹：声音号、采样率、帧数、循环开关。两次 Prepare 在同一个位置上
/// 指纹一样，就说明那一段没换，游标可以留着（见 `Prepare`）。
///
/// 用指纹而不是 `clip` 指针：重新 Load 一次之后，上一批 voice 里的指针已经指到别人身上了，读不得。
/// 四个数一起看，是因为单看声音号不够 —— 改了频率、长了、换了一段等长的声音，号都可能撞上。
std::uint64_t VoiceSignature(std::int32_t sound, const audio::Clip& clip, bool loop) {
    std::uint64_t value = 1469598103934665603ull;   // FNV-1a 起点
    const auto mix = [&value](std::uint64_t word) {
        value ^= word;
        value *= 1099511628211ull;
    };
    mix(static_cast<std::uint64_t>(sound));
    mix(clip.rate);
    mix(static_cast<std::uint64_t>(clip.frames.size()));
    mix(loop ? 1ull : 0ull);
    return value == 0 ? 1ull : value;               // 0 留给"没有声音"
}

}  // namespace

bool SoundRenderer::Prepare(const Scene& scene, std::string& error) {
    // **先把整份场景验完，再动旧的 voice。** 半路失败就保持原样，不上不下的一份更糟。
    std::vector<std::uint64_t> next_signatures;
    next_signatures.reserve(scene.emitters.size());

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

        std::uint64_t signature = 0;
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
            signature = VoiceSignature(emitter.sound, clip, emitter.loop);
        }
        next_signatures.push_back(signature);
    }

    // 拼下一批 voice。**放着同一样东西的，游标留着** —— 位置、音量、距离变了不该让声音从头来；
    // 换了一段、或者增删让序号挪了，那一个从头。
    std::vector<audio::Voice> next;
    next.reserve(scene.emitters.size());
    for (std::size_t index = 0; index < scene.emitters.size(); ++index) {
        const Emitter& emitter = scene.emitters[index];
        audio::Voice voice;
        if (emitter.sound != kNoSound) {
            const audio::Clip& clip = scene.sounds[emitter.sound];
            // A pointer into the scene's own clip: the scene outlives the voice, and a
            // copy per emitter would be a copy of the whole sound.
            voice.clip = &clip;
            voice.step = audio::StepFor(clip);
            voice.loop = emitter.loop;
            // 上一次在同一个位置上放的就是这一段吗？是就接着放。
            if (next_signatures[index] != 0 && index < voices_.size() &&
                index < signatures_.size() && signatures_[index] == next_signatures[index]) {
                voice.cursor = voices_[index].cursor;
                voice.finished = voices_[index].finished;
            }
        }
        next.push_back(voice);
    }

    voices_ = std::move(next);
    signatures_ = std::move(next_signatures);
    emitter_count_ = scene.emitters.size();

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

        // **Non-spatial emitters skip the two laws below entirely.** Background music is not
        // somewhere in the world: it is full gain and dead centre wherever the listener is. That
        // is the only difference, and it is why a song is played from one.
        std::array<audio::Sample, audio::kChannels> pan{centre, centre};
        audio::Sample level = 0.0f;
        if (!emitter.spatial) {
            level = static_cast<audio::Sample>(emitter.gain * scene.master_gain);
        } else {
            const Mat4 transform =
                emitter.parent == kNoParent ? emitter.local : world[emitter.parent] * emitter.local;
            const Vec3 position{transform.m[0][3], transform.m[1][3], transform.m[2][3]};

            const Vec3 to_emitter = position - scene.camera.eye;
            const float distance = std::sqrt(Dot(to_emitter, to_emitter));
            level = static_cast<audio::Sample>(emitter.gain * DistanceGain(emitter, distance) *
                                               scene.master_gain);
            if (panned && distance > 1.0e-6f) {
                pan = PanGains(Normalize(to_emitter), right);
            }
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
