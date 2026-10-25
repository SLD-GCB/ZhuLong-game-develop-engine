// 烛龙 (ZhuLong) - 一段合成音效的"配方"，以及把它渲染成样本的那一件事。
//
// 在这之前，一段声音只有三种来源：一个正弦（`MakeTone`）、一段白噪声（`MakeNoise`）、一个导入的
// WAV。前两个太素了——做不出音效。音效要的是**形状**：波形、音高的走向、音量的起落、以及音色上
// 的一点颜色。这个头就是那点形状。
//
// 它和 `MakeTone` / `MakeNoise` 是同一类东西：一份"描述"进去，一段 PCM 出来，**确定性**——同一个
// spec、同一个采样率，逐样本一样。所以它能被逐字节比较，也能在场景文本里只存 spec 不存样本
// （见 `scene_file.h` 的 `sound synth`）。
//
// 刻意窄，而且三处窄是明说的：
//   * 波形是**朴素**的（方/锯/三角直接由相位算），所以有混叠。短音效里听不出多少；band-limit 是
//     后来的旋钮，不是这里的债。
//   * 包络是**线性**的（和这一层"可预料"那条口味一致：线性衰减、线性重采样）。指数曲线也是后来的
//     旋钮——它让"手敲一个音"更好听，但不再是一眼算得出的形状。
//   * 只有一个**单极点低通**当"颜色"。一条滤波器栈、混响、延迟都不在这里。

#pragma once

#include <cstdint>
#include <string>

#include "zlong/audio/format.h"

namespace zlong::engine {

/// 一段合成音效的配方。它是一份**描述**，不是样本——所以能写进场景文本、能跟着场景一起撤销、
/// 能改一个数再渲染一遍，而样本本身永远只是它的结果（见 `RenderSynth`）。
struct SynthSpec {
    enum class Wave { Sine = 0, Square = 1, Saw = 2, Triangle = 3, Noise = 4 };

    Wave wave = Wave::Sine;
    /// 音高的起点和终点，Hz。相等就是不变；不等就是一次滑音——下滑的 zap、上升的 swoosh。
    /// **噪声没有音高**，这两个数对它没用。
    float start_hz = 440.0f;
    float end_hz = 440.0f;
    /// 多长，秒。
    float seconds = 0.3f;
    /// 幅度包络 ADSR——起音、衰减、持续电平、释音。前两个和最后一个是秒，`sustain` 是 0..1 的电平。
    ///
    /// **四段按长度铺满**：起音升到 1、衰减降到 `sustain`、保持、释音降到 0，加起来正好占满
    /// `seconds`（配得比长度还长就按比例缩）。所以"一段音效"永远是它自己那么长，不需要另一处写长度。
    float attack = 0.005f;
    float decay = 0.08f;
    float sustain = 0.5f;
    float release = 0.15f;
    /// 整段的输出增益。
    float gain = 1.0f;
    /// 单极点低通的截止频率，Hz。**0 = 不过滤**。这是"音色"那一档：噪声的闷与脆、声音的暗与亮。
    float cutoff_hz = 0.0f;
    /// 噪声的随机种子。定种子 ⇒ 同一段每次一样（和 `MakeNoise` 一个态度）。
    std::uint32_t seed = 1;
};

/// 把一份配方渲染成一段 PCM。**确定性**：同一份 spec、同一个采样率，逐样本一样。
///
/// `seconds <= 0` 得到一段空的（`Valid()` 为假），调用方照常处理——和 `MakeTone` 一样不抛。
audio::Clip RenderSynth(const SynthSpec& spec, std::uint32_t rate = audio::kSampleRate);

/// 波形的名字和号，两个方向。**这张表只应该有一份** —— 场景文本（`sound synth`）和曲子的
/// `.song` 文件都要它，第三处是 `编码器/main.py` 那个下拉框（那个在 Python 里，跑不开）。
const char* SynthWaveName(SynthSpec::Wave wave) noexcept;
bool SynthWaveFromName(const std::string& name, SynthSpec::Wave& out) noexcept;

}  // namespace zlong::engine
