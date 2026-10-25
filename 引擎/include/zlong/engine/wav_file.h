// 烛龙 (ZhuLong) - 把一个 WAV 文件读成一份 `audio::Clip`。
//
// `WavSink` 只出不进：它把混出来的样本写成文件，可从来没有人读回来过。这个头是另一半 ——
// 有了它，"做出来的声音"和"别人给的声音"才都能进到世界里来（`.scene` 的 `sound wav <路径>`），
// 就像 BMP 之于贴图、OBJ 之于网格。三件事是同一个形状：认出来一种，明确拒绝其余。
//
// **认 16 位 PCM（`wFormatTag == 1`）、单声道或双声道、任意采样率。** 别的——压缩格式（ADPCM、
// µ-law…）、IEEE float（格式 3）、`WAVE_FORMAT_EXTENSIBLE`（0xFFFE，多半是 24/32 位或更多声道）、
// 24/32 位整型、多于两个声道——一律拒绝并**说清它是哪一种**，因为一段声音读成噪声比读不出来坏得多。
//
// 单声道要摊成两声道：这一层的 `Frame` 就是两个采样（见 `audio::format.h`），而**铺到两边、
// 不是丢一半** —— 它本来就该从中间出来。
//
// 采样率不强行要 48k：`Mix` 那边本来就按 `StepFor(clip)` 做线性重采样，一个 44.1k 的素材放进来
// 是对的，不用在这儿先转一遍。
//
// 走 `file_io.h` 开文件（路径 UTF-8）—— 和 `scene_file.cpp`、`model_file.cpp` 一条路。

#pragma once

#include <string>

#include "zlong/audio/format.h"
#include "zlong/engine/file_io.h"

namespace zlong::engine {

/// 一段 WAV 的字节 → 一份 `audio::Clip`。认不出来就带上**为什么**（不说行号：这不是逐行的格式）。
/// 失败时 `out` 不动。
bool ParseWav(const std::string& bytes, audio::Clip& out, std::string& error);

/// 同一个，从文件读。路径是 UTF-8 —— 见 `file_io.h`。
bool LoadWav(const std::string& path, audio::Clip& out, std::string& error);

}  // namespace zlong::engine
