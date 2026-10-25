// 烛龙 (ZhuLong) - 一首曲子作为文本，像 `.model` 之于网格。
//
// 一份曲子是**一串音符过时间**，所以它比一段音效大得多（一件乐器、几十个音符），值得有自己的文件
// —— 场景里一行 `sound song 主题.song` 就够了，和 `model 椅子.model` 是同一种关系。曲子和它引用的
// 东西一起搬走也是同一套规矩。
//
// 格式是一行一条指令，空白分隔，`#` 起注释。第一行是版本（`song 1`）。见 `song.h` 里的
// `SongSpec` / `Instrument` / `Note`。

#pragma once

#include <string>

#include "zlong/engine/song.h"

namespace zlong::engine {

/// 一份曲子的文本 → `SongSpec`。认不出来就带上**行号和为什么**。
bool ParseSong(const std::string& text, SongSpec& out, std::string& error);

/// 读 `path` 再 `ParseSong`。路径是 UTF-8（见 `file_io.h`）。
bool LoadSong(const std::string& path, SongSpec& out, std::string& error);

/// 把一份曲子写到 `path`。**全写出来**（省掉的关键字这里一个不省），浮点用 9 位有效数字 —— 那是
/// float 的保证往返位数，六位会让每次存读都悄悄走样。
bool SaveSong(const std::string& path, const SongSpec& spec, std::string& error);

}  // namespace zlong::engine
