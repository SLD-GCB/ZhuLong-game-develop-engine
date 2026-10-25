// 烛龙 (ZhuLong) - a scene as text, so a scene is data and not code.
//
// This is the reason Scene is data at all: growing a scene should be a matter of
// editing this file, not of editing and recompiling a C++ program. The format is
// deliberately line-oriented and small -- one directive per line, whitespace
// separated, '#' begins a comment.
//
// The primitives are still built in (a box, a grid, a checkerboard, a tone); what the
// file describes is which of them a scene uses, how they are shaded, where they are,
// and what is heard from there. One file describes one world, so the same text drives
// the renderer and the sound renderer -- each reads the directives it understands and
// ignores the rest.
//
// **`model <一个 .model 的路径> X Y Z ROTY SCALE` 是唯一一条伸到文件外面去的指令**，而它是让
// "做出来的模型"能用上的那一条：建模器写出一个 `.model`，这一条把它读进来、按材质拆开、摆好。
// 路径是"除了最后五个数之外的全部"（所以它里面可以有空格），而且**相对场景文件自己的目录** ——
// 也正因为要相对某个目录，`ParseScene` 才多了一个 `base_dir`。

#pragma once

#include <array>
#include <string>
#include <vector>

#include "zlong/engine/scene.h"
#include "zlong/engine/synth.h"

namespace zlong::engine {

/// Parse a scene description into `out`, appending to whatever it already holds.
/// Returns false and fills `error` with the line number and the reason.
///
/// `base_dir` is the directory a relative `model` path is resolved against. A scene read from a
/// file passes its own directory (see `LoadScene`); a scene written out in code has no such thing,
/// so a `model` in `text` is then an **error** rather than a silent nothing — "the file is not
/// there" and "nobody told me where to look" are different problems.
bool ParseScene(const std::string& text, Scene& out, std::string& error,
                const std::string& base_dir = "");

/// Read `path`, then ParseScene it with `path`'s own directory as the base.
bool LoadScene(const std::string& path, Scene& out, std::string& error);

/// 一个要写出去的对象：一份**源形态**的网格、它的贴图路径相对谁、以及它摆在哪儿。
///
/// 导出成 `.scene` 的时候，网格各自落到场景旁边一个 `.model`，贴图拷到场景目录下、路径改成文件名
/// —— 于是"一个 `.scene` + 一堆 `.model` + 几张图"是一个能整包搬走的自包含目录。
struct SceneObjectInput {
    MeshDescription mesh;
    /// `mesh.textures[].path` 若为相对路径，相对谁。空 = 路径本身就能开（通常就是绝对路径）。
    std::string source_dir;
    /// 摆在哪儿、均匀缩放多少。**旋转已经烘进 `mesh` 的点了**（见建模器 `ExportScene`，因为 `.scene`
    /// 的摆放指令表达不了绕 X 转）；平移和缩放则是**原生**的 —— 指令本来就是 `T · Ry · S`。
    Vec3 position{0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

/// 要写出去的一段声音：**记住的是它的"源"，不是 PCM。**
///
/// `.scene` 里写的是一行 `sound tone …` / `sound wav …`，而从一段合成音最终的样本反推不回那一行
/// —— 所以写出器要的是"它是怎么来的"，就像网格那边要的是 `MeshDescription` 而不是烘好的 `Mesh`。
struct SceneSoundInput {
    enum class Kind { Tone, Noise, Wav, Synth, Song };
    Kind kind = Kind::Tone;
    /// tone：频率（Hz）。
    float frequency = 440.0f;
    /// tone / noise：多长（秒）。
    float seconds = 0.5f;
    /// noise：随机种子。**定死的种子**，所以两次导出是同一段噪声。
    std::uint32_t seed = 1;
    /// wav / song：文件名或路径。相对路径按 `source_dir` 解析。`song` 指的是一份 `.song`
    /// （`song.h`），读进来渲染成一段 —— 和 wav 一样，把文件拷到场景旁边。
    std::string path;
    /// wav / song 的 `path` 若相对，相对谁。空 = 路径本身就能开。
    std::string source_dir;
    /// synth：整份配方（波形 / 音高 / ADSR / 低通 ……）。**存的就是 spec**，写出去是一行
    /// `sound synth …`，读回来按同一份 spec 重新渲染 —— 样本本身不写进文件。
    SynthSpec synth;
};

/// 要写出去的一个声源：它放哪儿、放哪段声音、怎么衰减。摆在**世界坐标**里（`parent` 一般是
/// `kNoParent`）—— 工具里的声音是直接摆在世界里的，不是挂在某个节点上。
struct SceneEmitterInput {
    /// 第几个 `sounds`，或 `kNoSound`。
    std::int32_t sound = kNoSound;
    Vec3 position{0.0f, 0.0f, 0.0f};
    /// 挂到第几个**已经写出去的节点**上，或 `kNoParent`。工具里一律是世界坐标。
    std::int32_t parent = kNoParent;
    float gain = 1.0f;
    float reference_distance = 1.0f;
    float max_distance = 20.0f;
    bool loop = false;
    /// 写不写 `global`。为假时那个声源**不跟位置走**（满音量、居中）—— 背景音乐就是这样。
    bool spatial = true;
};

/// 要写出去的一份场景：世界（相机、灯光、环境、背景、阴影）+ 一串对象 + 一串声音。
struct SceneDescription {
    Camera camera;
    std::vector<Light> lights;
    Vec3 ambient{0.26f, 0.28f, 0.34f};
    std::array<float, 4> background{0.09f, 0.10f, 0.13f, 1.0f};
    std::uint32_t shadow_map_size = 0;
    std::vector<SceneObjectInput> objects;
    /// 听得见的东西。和对象一样，一份场景同时描述看和听。
    std::vector<SceneSoundInput> sounds;
    std::vector<SceneEmitterInput> emitters;
    /// 整场的音量，`master` 那一行。
    float master_gain = 1.0f;
};

/// 把一个场景写到 `path`：每个对象存成同级一个 `<主名>_<序号>.model`，贴图拷到同级、路径改成文件名，
/// 再写 `.scene` 本身。路径是 UTF-8（见 `file_io.h`）。
///
/// **只在能写完整的时候写**：某个对象的贴图找不到，就报错、不写 —— 一份"少了一张图"的场景，比一次
/// 明确的失败坏得多（和 `SaveModel` 一个态度）。
bool SaveScene(const std::string& path, const SceneDescription& description, std::string& error);

}  // namespace zlong::engine
