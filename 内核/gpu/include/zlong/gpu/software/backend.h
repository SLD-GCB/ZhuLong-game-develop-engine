// 烛龙 (ZhuLong) - the CPU render backend.
//
// This is not a placeholder: on a machine with no usable GPU it is what actually
// renders, and it is the always-available reference the Vulkan path is checked
// against.

#pragma once

#include <string>

#include "zlong/gpu/memory/gpu_memory.h"
#include "zlong/gpu/render/backend.h"
#include "zlong/gpu/software/pool.h"

namespace zlong::gpu::software {

class SoftwareBackend final : public render::RenderBackend {
public:
    explicit SoftwareBackend(GpuMemoryManager& memory);

    const char* name() const noexcept override { return "software"; }
    render::BackendCaps caps() const noexcept override;

    bool Initialize(std::string& error) override;
    void Shutdown() override;
    bool ready() const noexcept override { return true; }

    render::RenderResult Execute(const render::DrawDesc& draw) override;

    /// Always true: Execute is synchronous, so the pixels are already in guest
    /// memory by the time it returns.
    bool Flush(std::string& error) override;

    bool Supports(const render::DrawDesc& draw, std::string& why) const override;

private:
    GpuMemoryManager& memory_;
    /// Owned here rather than per draw: the rasterizer shades in parallel, and
    /// spawning threads for each of a scene's draws would cost more than it saves.
    ThreadPool pool_;
};

}  // namespace zlong::gpu::software
