// 烛龙 (ZhuLong) - 故宫: the palace, rendered from the courtyard and from the throne room.
//
// A host program like the others: it describes a scene and hands it to the machine. The point of
// this one is to answer the question the engine had never been asked -- can it render a *designed*
// 3D scene, rather than a field of boxes -- and to make the answer something that can be looked
// at rather than argued about.
//
// Vulkan is required rather than preferred: a scene this size through the software backend is
// seconds a frame, and the point here is the picture.

#include <chrono>
#include <cstdio>
#include <string>

#include "../render_scene/bmp.h"
#include "palace.h"
#include "zlong/system/system.h"

int main(int argc, char** argv) {
    std::string output;
    bool inside = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--inside") {
            inside = true;
        } else if (argument == "--out" && index + 1 < argc) {
            output = argv[++index];
        }
    }
    if (output.empty()) {
        output = inside ? "gugong_inside.bmp" : "gugong_outside.bmp";
    }

    zlong::system::System::Config config;
    config.vulkan = true;
    config.require_vulkan = true;
    config.width = 1280;
    config.height = 800;
    config.dram_bytes = 256ull * 1024 * 1024;
    config.arena_bytes = 64ull * 1024 * 1024;

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::fprintf(stderr, "the machine could not be built: %s\n", machine.error().c_str());
        return 1;
    }
    std::printf("backend: %s\n", machine.backend_name());

    zlong::engine::Scene scene;
    zlong::gugong::Build(scene, inside ? zlong::gugong::View::ThroneRoom
                                       : zlong::gugong::View::Courtyard);
    std::size_t drawables = 0;
    for (const auto& node : scene.nodes) {
        if (node.Drawable()) {
            ++drawables;
        }
    }
    std::printf("scene: %zu mesh(es), %zu texture(s), %zu material(s), %zu draw(s)\n",
                scene.meshes.size(), scene.textures.size(), scene.materials.size(), drawables);

    if (!machine.Load(scene)) {
        std::fprintf(stderr, "the palace could not be prepared: %s\n", machine.error().c_str());
        return 1;
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    // Two frames: the first pays for whatever the backend defers.
    if (machine.Frame() == zlong::system::System::FrameResult::Failed) {
        std::fprintf(stderr, "the palace could not be drawn: %s\n", machine.error().c_str());
        return 1;
    }
    const double first_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    if (machine.Frame() == zlong::system::System::FrameResult::Failed) {
        return 1;
    }

    const auto& engine = machine.renderer().profile();
    std::printf("frame: %.1f ms (prepare %.2f + constants %.2f + stream %.2f + submit %.2f)\n",
                first_ms, engine.prepare_ms, engine.constants_ms, engine.stream_ms,
                engine.submit_ms);
    const auto& device = machine.gpu().profile();
    std::printf("  device: decode %.2f + execute %.2f + flush %.2f ms, %zu fault(s)\n",
                device.decode_ms, device.execute_ms, device.flush_ms, machine.faults());

    if (!host::WriteBmp(output, machine.renderer().colour_pixels(), machine.renderer().width(),
                        machine.renderer().height(), machine.renderer().target_pitch())) {
        std::fprintf(stderr, "could not write %s\n", output.c_str());
        return 1;
    }
    std::printf("wrote %s (%ux%u)\n", output.c_str(), machine.renderer().width(),
                machine.renderer().height());
    return 0;
}
