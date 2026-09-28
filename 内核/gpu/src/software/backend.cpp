#include "zlong/gpu/software/backend.h"

#include <cstddef>
#include <thread>

#include "zlong/gpu/software/rasterizer.h"
#include "zlong/gpu/surface/format.h"

namespace zlong::gpu::software {

namespace {

/// Workers beyond the calling thread, so total parallelism is the core count.
/// Zero on a single-core machine, which leaves the rasterizer single-threaded.
std::size_t DefaultWorkerCount() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores > 1 ? static_cast<std::size_t>(cores) - 1 : 0;
}

}  // namespace

SoftwareBackend::SoftwareBackend(GpuMemoryManager& memory)
    : memory_(memory), pool_(DefaultWorkerCount()) {}

render::BackendCaps SoftwareBackend::caps() const noexcept {
    render::BackendCaps caps;
    caps.textures = true;
    caps.depth_test = true;
    caps.blending = true;
    caps.tiled_targets = true;
    caps.max_color_attachments = 1;
    return caps;
}

bool SoftwareBackend::Initialize(std::string& error) {
    error.clear();
    return true;
}

void SoftwareBackend::Shutdown() {}

bool SoftwareBackend::Flush(std::string& error) {
    error.clear();
    return true;
}

bool SoftwareBackend::Supports(const render::DrawDesc& draw, std::string& why) const {
    if (draw.vertex_shader == nullptr || draw.fragment_shader == nullptr) {
        why = "a vertex and a fragment shader are both required";
        return false;
    }
    if (draw.topology != render::PrimitiveTopology::Triangles) {
        why = "only a triangle list is implemented";
        return false;
    }
    if (!surface::IsColourTarget(draw.color.format)) {
        why = "the colour target format is not usable";
        return false;
    }
    if (draw.depth.has_value() && !surface::IsDepthTarget(draw.depth->format)) {
        why = "the depth target format is not usable";
        return false;
    }
    why.clear();
    return true;
}

render::RenderResult SoftwareBackend::Execute(const render::DrawDesc& draw) {
    std::string why;
    if (!Supports(draw, why)) {
        return render::RenderResult::Fail(render::RenderStatus::UnsupportedState, why);
    }
    Rasterizer rasterizer(draw, memory_, pool_);
    return rasterizer.Draw();
}

}  // namespace zlong::gpu::software
