// 烛龙 (ZhuLong) - the sound renderer: what turns a scene's emitters into frames.
//
// The renderer's other half, and the same split. The renderer walks the scene's nodes
// and turns the drawable ones into draws; this walks the same nodes and turns the
// emitters into voices, then hands a block of frames to the mixer. Both take the
// camera as the thing they are for -- for audio it is literally the ears.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "zlong/audio/format.h"
#include "zlong/audio/mixer.h"
#include "zlong/engine/scene.h"

namespace zlong::engine {

class SoundRenderer {
public:
    /// Checks the scene's emitters and gives each one a voice. Once per scene, like the
    /// renderer's Prepare: the sounds are already in memory, so this is validation and
    /// layout rather than an upload.
    ///
    /// **同一段声音接着放，不从头来。** 一个发射器如果还是放那一段（同一个声音、同一个循环
    /// 开关），它的播放位置**留着** —— 于是"改一下场景再 Load 一次"这种边改边看的用法不会每次
    /// 把声音从头重启（`System::Load` 就是被这样用的，见它那段）。哪一段声音换了、或者发射器的
    /// 增删让位置挪了，那一个从头来。
    bool Prepare(const Scene& scene, std::string& error);

    /// Mixes `frames.size()` frames and advances every emitter. Called as often as the
    /// caller likes -- the mix is continuous across calls, so a caller can hand it
    /// whatever block size its device asks for without a seam.
    bool Mix(const Scene& scene, std::span<audio::Frame> frames, std::string& error);

    /// How many emitters went into the last mix, silent placeholders included.
    std::size_t playing() const noexcept { return voices_.size(); }

private:
    std::vector<audio::Voice> voices_;
    /// 每个 voice 放的是"哪一段"的指纹（声音号 + 采样率 + 帧数 + 循环）。上一批和这一批在某个
    /// 位置上一样，那个 voice 的游标就留着 —— 见 `Prepare`。指纹而不是指针，是因为重新 Load 之后
    /// 上一批的 `clip` 指针已经指到别人身上去了。
    std::vector<std::uint64_t> signatures_;
    std::size_t emitter_count_ = 0;
};

}  // namespace zlong::engine
