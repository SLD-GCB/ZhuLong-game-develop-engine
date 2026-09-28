// 烛龙 (ZhuLong) - where mixed frames go.
//
// The render path's equivalent of RenderBackend: the mixer produces frames and
// something outside it decides what becomes of them. Keeping that seam here is what
// lets the same mix be written to a file, played on a device, or counted by a test.

#pragma once

#include <span>

#include "zlong/audio/format.h"

namespace zlong::audio {

class Sink {
public:
    virtual ~Sink() = default;

    /// Takes `frames`. Returns false when the sink could not take them -- a device
    /// that has gone away, a file that cannot be written.
    virtual bool Submit(std::span<const Frame> frames) = 0;

    /// Called once, when no more frames are coming, so a sink that has to finish
    /// something off (a file header, a device buffer) can. The render path's `Flush`
    /// is the same seam.
    virtual bool Finish() { return true; }

    /// What to call this sink in a diagnostic.
    virtual const char* name() const noexcept = 0;
};

}  // namespace zlong::audio
