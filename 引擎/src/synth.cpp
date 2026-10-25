#include "zlong/engine/synth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace zlong::engine {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/// 一个相位上的一个采样，取值大致 [-1, 1]。`noise` 用相位没用，改用那个 LCG——和 `MakeNoise`
/// 是同一个式子、同一个种子到同一个数。
float Waveform(SynthSpec::Wave wave, float phase, std::uint32_t& noise_state) {
    switch (wave) {
    case SynthSpec::Wave::Square:
        return phase < 0.5f ? 1.0f : -1.0f;
    case SynthSpec::Wave::Saw:
        return 2.0f * phase - 1.0f;
    case SynthSpec::Wave::Triangle:
        // |phase-0.5| 在 0 和 0.5 之间：两头 +1、中间 -1 的三角。
        return 4.0f * std::fabs(phase - 0.5f) - 1.0f;
    case SynthSpec::Wave::Noise:
        noise_state = noise_state * 1664525u + 1013904223u;
        // 高八位是它混得最好的那几位：[0, 2) 挪成 [-1, 1)。
        return static_cast<float>(noise_state >> 8) * (1.0f / 8388608.0f) - 1.0f;
    case SynthSpec::Wave::Sine:
    default:
        return std::sin(kTwoPi * phase);
    }
}

}  // namespace

audio::Clip RenderSynth(const SynthSpec& spec, std::uint32_t rate) {
    audio::Clip clip;
    clip.rate = rate == 0 ? audio::kSampleRate : rate;
    if (!(spec.seconds > 0.0f)) {
        return clip;   // 没有长度就没有样本；`Valid()` 会如实说
    }
    const auto frames = static_cast<std::size_t>(static_cast<double>(spec.seconds) * clip.rate);
    if (frames == 0) {
        return clip;
    }
    clip.frames.reserve(frames);
    const double span = static_cast<double>(frames);

    // 四段按长度铺满。加起来比长度还长就按比例缩 —— 这样 hand-written 的 A/D/R 不会把包络挤到
    // 长度外面去，而"一段音效多长"永远只由 `seconds` 一个数说了算。
    double attack = static_cast<double>(std::max(0.0f, spec.attack)) * clip.rate;
    double decay = static_cast<double>(std::max(0.0f, spec.decay)) * clip.rate;
    double release = static_cast<double>(std::max(0.0f, spec.release)) * clip.rate;
    const double sum = attack + decay + release;
    if (sum > span) {
        const double shrink = span / sum;
        attack *= shrink;
        decay *= shrink;
        release *= shrink;
    }
    const double sustain = std::clamp(static_cast<double>(spec.sustain), 0.0, 1.0);
    const double attack_end = attack;
    const double decay_end = attack_end + decay;
    const double hold_end = span - release;   // 释音从那以后开始，收在末尾
    const double sustain_end = std::max(decay_end, hold_end);

    // 单极点低通：y += k*(x - y)。cutoff <= 0 时 k = 1，也就是直通。
    float smoothing = 1.0f;
    if (spec.cutoff_hz > 0.0f) {
        const float limit = static_cast<float>(clip.rate) * 0.5f;   // 到奈奎斯特就够，再高没意义
        smoothing =
            1.0f - std::exp(-kTwoPi * std::min(spec.cutoff_hz, limit) / static_cast<float>(clip.rate));
    }

    float filtered = 0.0f;
    float phase = 0.0f;
    std::uint32_t noise_state = spec.seed == 0 ? 1u : spec.seed;
    for (std::size_t index = 0; index < frames; ++index) {
        const double at = static_cast<double>(index);

        // 幅度包络：起音升到 1、衰减降到 sustain、保持、释音降到 0。某一段长度为 0 就跳过它，
        // 于是 a=d=r=0 的配方得到一条平的（也就是旧 `MakeTone` 的那种）。
        double envelope = 1.0;
        if (at < attack_end) {
            envelope = attack > 0.0 ? at / attack : 1.0;
        } else if (at < decay_end) {
            envelope = decay > 0.0 ? 1.0 - (1.0 - sustain) * (at - attack_end) / decay : sustain;
        } else if (at < sustain_end) {
            envelope = sustain;
        } else {
            envelope = release > 0.0 ? sustain * (1.0 - (at - sustain_end) / release) : 0.0;
        }
        envelope = std::clamp(envelope, 0.0, 1.0);

        // 音高：从起点线性走到终点。走到哪儿按整段的比例算，所以换个长度滑音的形状不变。
        const double through = span > 0.0 ? at / span : 0.0;
        const double hz = static_cast<double>(spec.start_hz) +
                          (static_cast<double>(spec.end_hz) - static_cast<double>(spec.start_hz)) *
                              through;

        filtered += smoothing * (Waveform(spec.wave, phase, noise_state) - filtered);
        const auto value = static_cast<float>(filtered * envelope * spec.gain);
        clip.frames.push_back(audio::Frame{value, value});

        phase += static_cast<float>(hz / static_cast<double>(clip.rate));
        phase -= std::floor(phase);   // 相位始终留在 [0, 1)，方块/锯齿的边不会漂
    }
    return clip;
}

const char* SynthWaveName(SynthSpec::Wave wave) noexcept {
    switch (wave) {
    case SynthSpec::Wave::Square:
        return "square";
    case SynthSpec::Wave::Saw:
        return "saw";
    case SynthSpec::Wave::Triangle:
        return "triangle";
    case SynthSpec::Wave::Noise:
        return "noise";
    case SynthSpec::Wave::Sine:
    default:
        return "sine";
    }
}

bool SynthWaveFromName(const std::string& name, SynthSpec::Wave& out) noexcept {
    if (name == "sine") {
        out = SynthSpec::Wave::Sine;
    } else if (name == "square") {
        out = SynthSpec::Wave::Square;
    } else if (name == "saw") {
        out = SynthSpec::Wave::Saw;
    } else if (name == "triangle") {
        out = SynthSpec::Wave::Triangle;
    } else if (name == "noise") {
        out = SynthSpec::Wave::Noise;
    } else {
        return false;
    }
    return true;
}

}  // namespace zlong::engine
