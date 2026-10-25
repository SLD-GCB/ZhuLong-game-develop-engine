// 烛龙 (ZhuLong) - 一首曲子：音符过时间。
//
// 上一步把"一段声音"从"一份 PCM"变成了"一段能生成 PCM 的描述"（`SynthSpec`）。这一步再往前一步：
// 一首曲子是一串**音符**，每个音符有自己的起点、长度、音高和力度 —— 也就是**一个能渲染成 PCM 的
// 小程序**。
//
// 它和 `sound synth` / `sound wav` 是同一类东西：一个**产生 `Clip` 的东西**。所以 `Scene` /
// `SoundRenderer` / 混音器一个字都不用改 —— BGM 就是把它当一段声音交给一个发射器放（而且那种发射器
// 一般是**非空间**的：`Emitter::spatial`，音乐不该跟着听者走）。
//
// 刻意窄：
//   * **一件乐器就是一段 `SynthSpec` 去掉音高和长度** —— 那两样由音符给。所以 DSP 只有 `RenderSynth`
//     那一份，这里不新写一个采样器。
//   * **音符是固定音高**（没有 per-note 滑音）；没有段落、重复记号之类"曲式"的东西，一个循环段就是
//     一遍。
//   * **拍 + 速度（BPM）**，不是秒 —— 文件读起来像谱子，换算在渲染里（`SecondsPerBeat`）。

#pragma once

#include <cstdint>
#include <vector>

#include "zlong/audio/format.h"
#include "zlong/engine/synth.h"

namespace zlong::engine {

/// 一件乐器：一段合成音效里"除了音高和多长之外"的那些东西。音高由音符给、多长由音符给 —— 所以这里
/// 没有 `start_hz` / `end_hz` / `seconds`。
///
/// 包络按**每个音符自己的长度**铺满（`RenderSynth` 那条规矩），所以一件"长笛"（起音慢、持续）和一件
/// "拨弦"（起音快、衰减到底）差别就在这四个数上。
struct Instrument {
    SynthSpec::Wave wave = SynthSpec::Wave::Sine;
    float attack = 0.01f;
    float decay = 0.10f;
    float sustain = 0.70f;
    float release = 0.20f;
    float gain = 0.6f;
    float cutoff_hz = 0.0f;
    std::uint32_t seed = 1;
};

/// 一个音符：第几拍起、多长（拍）、哪个音（MIDI 号）、多响（0..1）、哪件乐器。
struct Note {
    double start_beats = 0.0;
    double beats = 1.0;
    /// MIDI 号。**60 = 中央 C，69 = A4 = 440 Hz** —— 音名和 Hz 的关系见 `PitchHz`。
    int pitch = 60;
    float velocity = 1.0f;
    /// 第几件乐器（`SongSpec::instruments` 的下标）。
    int instrument = 0;
};

/// 一首曲子：速度、一遍多长（拍）、几件乐器、一串音符。
struct SongSpec {
    double tempo_bpm = 120.0;
    /// 一遍多少拍。**它就是"循环长度"** —— 渲染出来的 `Clip` 正好这么长，`loop` 交给发射器。
    double loop_beats = 8.0;
    std::vector<Instrument> instruments;
    std::vector<Note> notes;

    /// 一拍多少秒。
    double SecondsPerBeat() const noexcept { return tempo_bpm > 0.0 ? 60.0 / tempo_bpm : 0.5; }
    /// 一遍多少秒 —— 也就是渲染出来那段有多长。
    double Seconds() const noexcept { return loop_beats * SecondsPerBeat(); }
};

/// MIDI 号 → 频率，十二平均律。拎出来是因为"音名和 Hz 的关系"是可以单独验的一件事。
double PitchHz(int pitch) noexcept;

/// 把一份曲子渲染成一段 PCM。**确定性**（和 `RenderSynth` 一样）：同一个 spec、同一个采样率，逐样本
/// 一样 —— 所以曲子能只存 `.song` 文本、不存样本。
///
/// 逐音符调 `RenderSynth`（把乐器的波形/包络和音符的音高/长度拼成一份 per-note spec），结果**相加**；
/// 越过一遍末尾的那截**绕回开头**，所以循环的接缝不会"啪"一声。相加不夹 —— 夹是 sink 的事。
///
/// 没有音符（或者一遍的长度非正）得到一段空的，调用方照常处理。
audio::Clip RenderSong(const SongSpec& spec, std::uint32_t rate = audio::kSampleRate);

}  // namespace zlong::engine
