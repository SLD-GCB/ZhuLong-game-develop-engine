// 烛龙 (ZhuLong) - the render backend interface.
//
// Two real implementations exist: a Vulkan backend and a CPU (software)
// rasterizer. Neither is a stub: on a machine with no usable GPU the software
// backend is what actually renders.

#pragma once

#include <cstdint>
#include <string>

#include "zlong/gpu/render/draw.h"

namespace zlong::gpu::render {

struct BackendCaps {
    bool textures = false;
    bool depth_test = false;
    bool blending = false;
    bool tiled_targets = false;
    std::uint32_t max_color_attachments = 1;
};

enum class RenderStatus : std::uint8_t {
    Ok,
    UnsupportedState,
    BadTarget,
    BadGeometry,
    ShaderError,
    DeviceLost,
    Internal,
};

struct RenderResult {
    RenderStatus status = RenderStatus::Ok;
    std::string detail;

    bool ok() const noexcept { return status == RenderStatus::Ok; }

    static RenderResult Good() { return {}; }
    static RenderResult Fail(RenderStatus status, std::string detail) {
        return RenderResult{status, std::move(detail)};
    }
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    /// "vulkan" or "software".
    virtual const char* name() const noexcept = 0;
    virtual BackendCaps caps() const noexcept = 0;

    /// Returns false and fills `error`; never throws.
    virtual bool Initialize(std::string& error) = 0;
    virtual void Shutdown() = 0;
    virtual bool ready() const noexcept = 0;

    /// Record one draw. Called only on the render worker thread, and runs to
    /// completion.
    ///
    /// The pixels are NOT guaranteed to be in guest memory when this returns: a
    /// backend is free to keep a run of draws to the same target resident and
    /// publish them together, which is what stops a 65-draw frame from
    /// round-tripping its target 65 times. Flush is what makes the result visible.
    virtual RenderResult Execute(const DrawDesc& draw) = 0;

    /// Publish prior work into guest RAM and block until it is there. A caller that
    /// reads a target it just drew into must call this first. The software backend
    /// publishes as it goes, so this is a no-op there.
    virtual bool Flush(std::string& error) = 0;

    /// Pure capability check, so the engine can report "this state is not
    /// supported" instead of silently mis-rendering.
    virtual bool Supports(const DrawDesc& draw, std::string& why) const = 0;
};

}  // namespace zlong::gpu::render
