// 烛龙 (ZhuLong) - 把一张图片文件读成一份 `Texture`。
//
// 读的是 **BMP**，因为它是这一类格式里唯一一个"不压缩、没有专利、哪儿都存得出来"的。一份 `Texture`
// 就是 RGBA8 紧排、**第一行在上**（见 `scene.h`），所以这里做的事可以一句话说完：把文件里的像素
// 摊成那个样子。
//
// **认 24 位和 32 位、不压缩（`BI_RGB`）的那些。** 别的——1/4/8 位的调色板、RLE 压缩、显式掩码的
// `BI_BITFIELDS`、老式的 `BITMAPCOREHEADER`——一律拒绝并说清是哪一种。这不是偷懒：一张图读成花花
// 绿绿，比读不出来坏得多，而"读不出来"至少还知道要换一张。
//
// 两件真实世界里必须处理的：
//
//   * **行是倒着的**，除非高度写成了负数（那时是正着的）。**两个都要认**，而且它们印出来的图一样，
//     所以搞错了不会报错，只会上下颠倒。
//   * **每行按 4 字节对齐**，所以宽度是奇数的时候行尾有填充字节 —— 不跳过它，图会斜着扭曲。
//
// 32 位那一档还有一个约定：第 4 个字节在 `BI_RGB` 里按标准是"保留"，但很多写图的工具把 alpha 写在
// 那儿，而另一些工具一律写 0。所以**全 0 就当作不透明**（否则每一张 32 位的图都会是透明的）。

#pragma once

#include <string>

#include "zlong/engine/file_io.h"
#include "zlong/engine/scene.h"

namespace zlong::engine {

/// 一张 BMP 的字节 → 一份 `Texture`。认不出来就带上**为什么**（不说行号：这不是逐行的格式）。
bool ParseBmp(const std::string& bytes, Texture& out, std::string& error);

/// 同一个，从文件读。路径是 UTF-8 —— 见 `file_io.h`。
bool LoadBmp(const std::string& path, Texture& out, std::string& error);

}  // namespace zlong::engine
