// 烛龙 (ZhuLong) - 一个把帧丢掉、只数一数的 sink。
//
// 没有设备的时候（别的平台、没插声卡、或者声卡被别人占着），机器仍然要能混音 ——
// `System::Mix` 在 `audio_ == nullptr` 时是直接报错的，而一个"想听却听不到"的工具不该因此整块
// 立不起来。有了它，画面照常走、`Mix` 照常被调，只是听不见；而"混了多少帧"仍然是一笔对得上的数。
//
// 它也是 `sink.h` 那段话里说的"测试用的计数器"：一个 mix 去文件、去声卡、还是去一个计数，对
// mixer 来说没有区别。
//
// 头文件级的小东西，不占一个翻译单元 —— 和 `format.h` 里的几个换算一样。

#pragma once

#include <cstdint>
#include <span>

#include "zlong/audio/sink.h"

namespace zlong::audio {

class NullSink final : public Sink {
public:
    bool Submit(std::span<const Frame> frames) override {
        frames_ += frames.size();
        return true;
    }

    const char* name() const noexcept override { return "null"; }

    /// 一共吞了多少帧。没有声音的时候，这是"混音真的在跑"的唯一凭证。
    std::uint64_t frames() const noexcept { return frames_; }

private:
    std::uint64_t frames_ = 0;
};

}  // namespace zlong::audio
