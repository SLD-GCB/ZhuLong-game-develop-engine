// 烛龙 (ZhuLong) - 一个模型存成文本，于是模型是数据，而不是一次会话。
//
// 和 `scene_file.h` 是同一套想法、同一套写法：一行一条指令，空白分隔，`#` 起注释。分成两个文件
// 是因为它们是两种东西：一个**场景**是摆出来的（相机、灯光、几样东西放在哪），一个**模型**是捏
// 出来的（点，和指着点的面），而且模型大得多，还要能被几个场景共用。
//
//     mesh 4
//     material 0.62 0.64 0.68 1 0.35              没有贴图的
//     material 0.90 0.20 0.20 1 0.30 木头.bmp     有贴图的，路径在最后
//     v -0.500000 -0.500000 -0.500000
//     f flat   4  0  0 3 2 1   0 0   1 0   1 1   0 1
//     crease 0 3                                  一条硬边（细分的时候它不跟着变圆）
//
// **uv 写在面上，不写在点上** —— 这不是排版方便。一个点是空间里的一个位置，几个面共用它；而 uv
// 说的是"这个面的这个角贴到贴图的哪儿"，同一个位置在不同的面上可以贴在贴图的不同地方。这正是
// 那份描述**焊接**起来的意义，也正是 `BakeMesh` 要按角去拆的原因。把 uv 挪到点上去，就等于把那份
// 焊接偷偷拆了。
//
// **硬边是单独写出来的，一行一条 `crease`**（低号在前）。一条边本身没有属性可记（它是两个面共用
// 出来的），要记下来的那件事只是"这条边是硬的"，所以它不挂在面或者角上。没有硬边的文件一行
// `crease` 也没有 —— 于是**一个 `mesh 3` 的老文件照样读得进来**，只是没有硬边。
//
// **贴图的路径是相对这个 `.model` 自己**，和场景里 `model` 那条指令一个道理 —— 所以贴图和模型可以
// 整包一起挪走。也正因为要相对某个目录，`ParseModel` 才有一个 `base_dir`：没有它的时候，一条写了
// 贴图的 `material` 是**报错**，而不是当没看见。
//
// `mesh 4` 那个版本号在文件里：改了格式，读的人有机会说"这是老版本的"，而不是把一份文件读成一堆
// 看着像模像样的点。（1 和 2 都不认了 —— 这份格式还没有别人写的文件；3 认，因为它只差 `crease`。）

#pragma once

#include <cstdint>
#include <string>

#include "zlong/engine/file_io.h"
#include "zlong/engine/mesh.h"

namespace zlong::engine {

/// 一份描述变成文本（不碰文件）。`f` 里的 uv 按角写，就上面那样的形状。
std::string WriteModel(const MeshDescription& description);

/// 从文本读一份描述。
///
/// 认不出来的行、对不上的数字、指不到点上的角，都会带上**行号**说不来 —— 这个格式是给人手改的，
/// 所以"第 12 行这个角是 99，只有 8 个点"比"读不了"有用得多。
///
/// `base_dir` 是 `material` 里那条贴图路径**相对谁**。从文件读的时候是它的目录（见 `LoadModel`）；
/// 写在代码里的时候没有这么个东西，那时候一条带贴图的 `material` 是报错。
bool ParseModel(const std::string& text, MeshDescription& out, std::string& error,
                const std::string& base_dir = "");

/// 从文件读 / 写到文件。路径是 UTF-8 —— 中文路径能走通，见 `file_io.h`。
bool LoadModel(const std::string& path, MeshDescription& out, std::string& error);
bool SaveModel(const std::string& path, const MeshDescription& description, std::string& error);

}  // namespace zlong::engine
