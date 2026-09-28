// 烛龙 (ZhuLong) - 这一层怎么开文件。
//
// 一份，两个读者用（`scene_file.cpp` 和 `model_file.cpp`），所以它自己一对文件，而不是某个读者
// 顺手带着。名字不起眼是因为它管的就这一件事 —— 但它是**踩出来的**，见 `OpenForRead` 上面那段。

#pragma once

#include <cstdio>
#include <string>

namespace zlong::engine {

/// 打开一个文件来读 / 写。路径是 **UTF-8**；开不了就返回 `nullptr`。
///
/// **这里为什么不用标准库那两条路**（两条都试过）：
///
/// * `std::ifstream(std::string)` —— Windows 上它按 **ANSI 代码页**解释那串字节，
///   `烛龙\我的模型.model` 会被读成别的字，然后报"打不开"，而那个文件就好好地在那儿。
/// * `std::filesystem::path(std::u8string(...))` 再交给 ifstream —— libc++ 在 Windows 上转过
///   非 ASCII 的时候**直接抛** `filesystem_error: in __char_to_wide: Illegal byte sequence`，
///   而且没人接，进程就终止了。
///
/// 所以这里自己转：`MultiByteToWideChar(CP_UTF8)` 加 `_wfopen`。这条路是这个平台上唯一一条确定
/// 走得通的，而它值得单独摆在这里，因为**这个项目和它做出来的东西都住在中文目录下**。
std::FILE* OpenForRead(const std::string& utf8_path);
std::FILE* OpenForWrite(const std::string& utf8_path);

/// 一个文件整个读进来。失败（开不了、读到一半出错）返回 false，`contents` 不动。
bool ReadWholeFile(const std::string& utf8_path, std::string& contents);

/// 一个字符串整个写出去。**写完要能确认写完** —— 磁盘满了和写好了是两件事，而一个"看起来存过
/// 了"的模型比一次明确的失败坏得多。
bool WriteWholeFile(const std::string& utf8_path, const std::string& contents);

}  // namespace zlong::engine
