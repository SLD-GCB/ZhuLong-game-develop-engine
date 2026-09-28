// 烛龙 (ZhuLong) - build a scene and render it to a file.
//
// A host program, and deliberately thin: the machine is 内核/system and the engine is
// 引擎/. This describes a scene, hands it over, and writes the frame it drew. There is
// no loop here and no GPU handle -- the kernel owns those (see System), which is the
// point of the layer: a host describes content, it does not assemble a machine.
//
// A scene can also come from a file (`--scene path`), which is the point of the engine's
// Scene being data: growing it is editing the file, not this program.

#include <cstdint>
#include <cstdio>
#include <string>

#include "bmp.h"
#include "zlong/engine/scene_file.h"
#include "zlong/system/system.h"

namespace {

using zlong::engine::Light;
using zlong::engine::Material;
using zlong::engine::RotationY;
using zlong::engine::Scale;
using zlong::engine::Scene;
using zlong::engine::Translation;
using zlong::engine::Vec3;

constexpr std::uint32_t kCheckerTexels = 64;
constexpr std::uint32_t kCheckerCells = 8;
constexpr std::uint32_t kRippleCells = 4;
constexpr std::uint32_t kShadowMapSize = 512;

/// The scene this program renders when no file is given: a ground, a tower, a box
/// stacked on the tower (a child node, so it inherits the tower's turn), a loose box,
/// and one behind the camera that the cull must drop.
///
/// The angles are whole degrees so the file version of this same scene lands on exactly
/// the same rotation and the two renders can be compared bit for bit.
Scene BuiltInScene() {
    Scene scene;
    scene.camera.eye = Vec3{0.0f, 2.8f, 6.2f};
    scene.camera.target = Vec3{0.0f, 0.6f, 0.0f};
    scene.background = {0.05f, 0.06f, 0.10f, 1.0f};

    scene.meshes.push_back(zlong::engine::MakeGrid(1));
    scene.meshes.push_back(zlong::engine::MakeBox());
    scene.textures.push_back(zlong::engine::MakeCheckerTexture(kCheckerTexels, kCheckerCells));
    scene.textures.push_back(zlong::engine::MakeRippleNormalTexture(kCheckerTexels, kRippleCells));

    // A warm key and a dim cool fill from the other side, so a surface facing away from
    // the key is not flat black.
    Light key;
    key.direction = Vec3{0.45f, 0.80f, 0.40f};
    key.colour = Vec3{0.85f, 0.82f, 0.75f};
    scene.lights.push_back(key);

    Light fill;
    fill.direction = Vec3{-0.50f, 0.25f, -0.60f};
    fill.colour = Vec3{0.18f, 0.22f, 0.35f};
    scene.lights.push_back(fill);

    scene.ambient = Vec3{0.18f, 0.19f, 0.24f};
    scene.shadow_map_size = kShadowMapSize;

    // The ground is textured, normal-mapped and matte; the boxes are tinted and glossy
    // with no normal map.
    Material ground;
    ground.texture = 0;
    ground.normal_texture = 1;
    ground.specular = 0.05f;
    scene.materials.push_back(ground);

    const auto glossy = [&](float r, float g, float b) {
        Material material;
        material.tint = {r, g, b, 1.0f};
        material.specular = 0.45f;
        scene.materials.push_back(material);
    };
    glossy(0.85f, 0.35f, 0.30f);
    glossy(0.30f, 0.70f, 0.40f);
    glossy(0.35f, 0.50f, 0.90f);

    scene.AddNode(0, 0, Scale(24.0f));
    const std::int32_t tower =
        scene.AddNode(1, 1, Translation(-2.0f, 0.7f, 0.3f) * RotationY(zlong::engine::Radians(30.0f)) *
                                Scale(1.4f));
    scene.AddNode(1, 2, Translation(0.0f, 1.0f, 0.0f) * Scale(0.6f), tower);
    scene.AddNode(1, 3, Translation(2.3f, 0.45f, 0.6f) *
                            RotationY(zlong::engine::Radians(120.0f)) * Scale(0.9f));
    scene.AddNode(1, 3, Translation(0.0f, 0.5f, 20.0f) * Scale(1.5f));
    return scene;
}

}  // namespace

int main(int argc, char** argv) {
    std::string output = "scene.bmp";
    std::string scene_path;
    zlong::system::System::Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--scene" && index + 1 < argc) {
            scene_path = argv[++index];
        } else if (argument == "--vulkan") {
            config.vulkan = true;
        } else {
            output = argument;
        }
    }

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::fprintf(stderr, "the machine could not be built: %s\n", machine.error().c_str());
        return 1;
    }
    if (!machine.note().empty()) {
        std::fprintf(stderr, "%s\n", machine.note().c_str());
    }
    std::printf("backend: %s\n", machine.backend_name());

    // --- the scene, as data ------------------------------------------------
    std::string error;
    Scene scene;
    if (scene_path.empty()) {
        scene = BuiltInScene();
    } else if (!zlong::engine::LoadScene(scene_path, scene, error)) {
        std::fprintf(stderr, "the scene %s could not be loaded: %s\n", scene_path.c_str(),
                     error.c_str());
        return 1;
    }

    // --- render -------------------------------------------------------------
    if (!machine.Load(scene)) {
        std::fprintf(stderr, "the scene could not be prepared: %s\n", machine.error().c_str());
        return 1;
    }
    if (machine.Frame() == zlong::system::System::FrameResult::Failed) {
        std::fprintf(stderr, "the scene could not be rendered: %s\n", machine.error().c_str());
        return 1;
    }

    std::size_t drawables = 0;
    for (const auto& node : machine.scene().nodes) {
        if (node.Drawable()) {
            ++drawables;
        }
    }
    std::printf("submitted %zu of %zu drawable(s), %zu draw(s) total, %zu gpu fault(s)\n",
                machine.renderer().submitted(), drawables, machine.draws(), machine.faults());
    for (const auto& fault : machine.fault_log()) {
        std::fprintf(stderr, "  gpu fault (kind %d): %s\n", static_cast<int>(fault.kind),
                     fault.detail.c_str());
    }

    const std::uint8_t* pixels = machine.renderer().colour_pixels();
    if (pixels == nullptr) {
        std::fprintf(stderr, "the colour target is not readable\n");
        return 1;
    }
    if (!host::WriteBmp(output, pixels, machine.renderer().width(), machine.renderer().height(),
                        machine.renderer().target_pitch())) {
        std::fprintf(stderr, "could not write %s\n", output.c_str());
        return 1;
    }
    std::printf("wrote %s (%ux%u)\n", output.c_str(), machine.renderer().width(),
                machine.renderer().height());
    return 0;
}
