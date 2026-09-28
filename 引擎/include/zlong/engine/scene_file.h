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

#include <string>

#include "zlong/engine/scene.h"

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

}  // namespace zlong::engine
