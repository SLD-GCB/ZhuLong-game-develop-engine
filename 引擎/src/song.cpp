#include "zlong/engine/song.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace zlong::engine {

double PitchHz(int pitch) noexcept {
    // 十二平均律：A4（69）= 440 Hz，往上升一个半音乘 2^(1/12)。
    return 440.0 * std::pow(2.0, (static_cast<double>(pitch) - 69.0) / 12.0);
}

audio::Clip RenderSong(const SongSpec& spec, std::uint32_t rate) {
    audio::Clip clip;
    clip.rate = rate == 0 ? audio::kSampleRate : rate;

    const double seconds = spec.Seconds();
    if (!(seconds > 0.0) || spec.notes.empty()) {
        return clip;   // 没有一遍的长度，或者没有音符：一段空的，`Valid()` 会如实说
    }
    const auto total = static_cast<std::size_t>(seconds * static_cast<double>(clip.rate));
    if (total == 0) {
        return clip;
    }
    // 先铺一层静音，再把每个音符加进去 —— 相加而不是覆盖，所以和弦和叠音是白送的。
    clip.frames.assign(total, audio::Frame{0.0f, 0.0f});

    const double seconds_per_beat = spec.SecondsPerBeat();
    for (const Note& note : spec.notes) {
        if (note.instrument < 0 ||
            static_cast<std::size_t>(note.instrument) >= spec.instruments.size()) {
            continue;   // 解析器会拦下来；这里再拦一次，好让手搓的 spec 也不越界
        }
        if (!(note.beats > 0.0)) {
            continue;
        }
        const Instrument& instrument = spec.instruments[static_cast<std::size_t>(note.instrument)];

        // 一件乐器 + 一个音符 = 一份 per-note 的合成配方。**DSP 只有 `RenderSynth` 那一份。**
        SynthSpec one;
        one.wave = instrument.wave;
        const auto hertz = static_cast<float>(PitchHz(note.pitch));
        one.start_hz = hertz;
        one.end_hz = hertz;
        one.seconds = static_cast<float>(note.beats * seconds_per_beat);
        one.attack = instrument.attack;
        one.decay = instrument.decay;
        one.sustain = instrument.sustain;
        one.release = instrument.release;
        one.gain = instrument.gain * std::max(0.0f, note.velocity);
        one.cutoff_hz = instrument.cutoff_hz;
        one.seed = instrument.seed;

        const audio::Clip rendered = RenderSynth(one, clip.rate);
        const auto start = static_cast<std::size_t>(
            std::llround(note.start_beats * seconds_per_beat * static_cast<double>(clip.rate)));
        for (std::size_t index = 0; index < rendered.frames.size(); ++index) {
            // **绕回开头**：一段会循环的曲子，末尾那个音符的尾巴要接在开头 —— 不能"啪"一声。
            const std::size_t at = (start + index) % total;
            clip.frames[at][0] += rendered.frames[index][0];
            clip.frames[at][1] += rendered.frames[index][1];
        }
    }
    return clip;
}

}  // namespace zlong::engine
