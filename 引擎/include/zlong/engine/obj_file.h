// 烛龙 (ZhuLong) - 把别人的网格文件读成一份 `MeshDescription`。
//
// 读的是 Wavefront OBJ，因为它是这一类文件里唯一一个"文本、几乎没有语法、哪儿都导得出来"的。
// 读进来之后它就和其他任何一个模型一样了 —— 同一个类型、同一个烘、同一套编辑操作。**导入不是
// 第二条路，是另一个生产者**。
//
// 认这些：
//
//     v x y z                     点
//     vt u v                      uv
//     vn x y z                    法线（**读，但不用** —— 见下）
//     f  a  b  c                  面，角可以写成 a、a/b、a//c、a/b/c，下标从 1 数，可以是负的
//     s off | s 1                 这一片是不是平滑的
//     o / g / usemtl / mtllib     认得出，但只跳过
//     #                           注释
//
// **uv 是每个角一个**，和 OBJ 的 `vt` 天然对得上 —— OBJ 本来就不焊：点是点、uv 是 uv，一个角是
// 一对下标。所以这里不用"按缝拆点"，模型那份表示已经是那个形状了。
//
// **法线读了不用。** 这个引擎的法线是烘出来的（面法线，或者平滑的时候取邻域平均），顶点上没有
// 一条能塞 `vn` 的地方。所以"这个面是平是滑"**按 `s` 判断**，文件里没有 `s` 就一律平 —— 那条
// 规则至少是可预期的，而拿 `vn` 去倒推是猜。
//
// **材质不在这份东西里。** `usemtl` 跳过的原因不是省事：一个模型的材质是这个模型被摆进某个场景
// 之后那一步的事（`Scene::materials` + `Node`），把它塞进网格里会有第二个说法。

#pragma once

#include <cstddef>
#include <string>

#include "zlong/engine/file_io.h"
#include "zlong/engine/mesh.h"

namespace zlong::engine {

/// 读的时候发生了什么。
///
/// **这份东西存在，是因为导入一定会改变文件。** 一个六个角的面要变成四个三角形，文件里没被任何
/// 面用到的点要丢掉 —— 这些都不该悄悄发生。读的人拿着这份账去说"你的模型进来之后是什么样"。
struct ImportReport {
    std::size_t points_in_file = 0;     // 文件里有多少个 v
    std::size_t points_kept = 0;        // 其中有多少进了模型
    std::size_t points_dropped = 0;     // 没被任何面用到、丢掉的
    std::size_t faces_in_file = 0;      // 文件里有多少个 f
    std::size_t faces_fanned = 0;       // 其中五个角以上的，被扇成三角形
    std::size_t faces_flat = 0;         // 平 / 滑各多少
    std::size_t faces_smooth = 0;
    std::size_t lines_ignored = 0;      // 认得出但用不上的行（物体名、材质、画线画点那些）
};

/// OBJ 文本 → 一份描述。认不出来的行（以及为什么）带上**行号**。
bool ParseObj(const std::string& text, MeshDescription& out, ImportReport& report,
              std::string& error);

/// 同一个，从文件读。路径是 UTF-8 —— 见 `file_io.h`。
bool LoadObj(const std::string& path, MeshDescription& out, ImportReport& report,
             std::string& error);

}  // namespace zlong::engine
