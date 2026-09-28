// 烛龙 (ZhuLong) - the machine, as a Python module.
//
// The bridge between the C++ backend and the Qt front end *is* the System layer: the front end is
// not given a protocol to speak, it is given this class. That is why the surface here is narrow on
// purpose -- `System`, `Scene` and the things a `Platform` has to name, and nothing below them. The
// kernel, the GPU and the Vulkan context are the machine's business; a front end that could reach
// into them would break every time one of them changed, which is the thing a bridge is supposed to
// prevent.
//
// The front end implements `Platform` in Python. `present` is handed a *copy* of the frame, because
// the real buffer belongs to the machine and the next frame overwrites it.

#include <cstddef>
#include <cstdint>
#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "zlong/engine/mesh.h"
#include "zlong/engine/scene.h"
#include "zlong/system/system.h"

namespace py = pybind11;

namespace {

using zlong::engine::Camera;
using zlong::engine::Light;
using zlong::engine::Material;
using zlong::engine::Mesh;
using zlong::engine::Node;
using zlong::engine::Scene;
using zlong::engine::Texture;
using zlong::engine::Vec3;
using zlong::system::Platform;
using zlong::system::System;

/// Lets a Python class be a Platform: the machine calls into it, and this forwards.
class PyPlatform : public Platform {
public:
    using Platform::Platform;

    bool Pump() override { PYBIND11_OVERRIDE(bool, Platform, Pump); }

    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                 std::uint32_t height) override {
        py::gil_scoped_acquire gil;
        py::function override = py::get_override(static_cast<Platform*>(this), "present");
        if (!override) {
            return;
        }
        // A copy: the buffer is the machine's, and the next frame writes over it.
        const auto bytes = static_cast<std::size_t>(pitch) * height;
        py::bytes frame(reinterpret_cast<const char*>(rgba), bytes);
        override(frame, pitch, width, height);
    }

    void OnFrame(Scene& scene, double seconds) override {
        py::gil_scoped_acquire gil;
        py::function override = py::get_override(static_cast<Platform*>(this), "on_frame");
        if (!override) {
            return;
        }
        override(std::ref(scene), seconds);
    }

    void OnMix(Scene& scene, double seconds) override {
        py::gil_scoped_acquire gil;
        py::function override = py::get_override(static_cast<Platform*>(this), "on_mix");
        if (!override) {
            return;
        }
        override(std::ref(scene), seconds);
    }
};

}  // namespace

PYBIND11_MODULE(zlong, m) {
    m.doc() = "The 烛龙 machine: a kernel, a GPU, an engine and the loop that drives them.";

    py::class_<Vec3>(m, "Vec3")
        .def(py::init([](float x, float y, float z) { return Vec3{x, y, z}; }), py::arg("x") = 0.0f,
             py::arg("y") = 0.0f, py::arg("z") = 0.0f)
        .def_readwrite("x", &Vec3::x)
        .def_readwrite("y", &Vec3::y)
        .def_readwrite("z", &Vec3::z)
        .def("__repr__", [](const Vec3& v) {
            return "Vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " +
                   std::to_string(v.z) + ")";
        });

    py::class_<Camera>(m, "Camera")
        .def(py::init<>())
        .def_readwrite("eye", &Camera::eye)
        .def_readwrite("target", &Camera::target)
        .def_readwrite("up", &Camera::up)
        .def_readwrite("fov_y_radians", &Camera::fov_y_radians)
        .def_readwrite("near_z", &Camera::near_z)
        .def_readwrite("far_z", &Camera::far_z);

    py::class_<Light>(m, "Light")
        .def(py::init<>())
        .def_readwrite("direction", &Light::direction)
        .def_readwrite("colour", &Light::colour);

    py::class_<Texture>(m, "Texture")
        .def_readonly("width", &Texture::width)
        .def_readonly("height", &Texture::height);

    py::class_<Material>(m, "Material")
        .def(py::init<>())
        // A list rather than a std::array member: an array crosses into Python as a copy, so
        // `material.tint[0] = 1` would change a list nobody keeps.
        .def_property(
            "tint",
            [](const Material& material) {
                return std::vector<float>(material.tint.begin(), material.tint.end());
            },
            [](Material& material, const std::vector<float>& tint) {
                for (std::size_t index = 0; index < material.tint.size() && index < tint.size();
                     ++index) {
                    material.tint[index] = tint[index];
                }
            })
        .def_readwrite("specular", &Material::specular)
        .def_readwrite("texture", &Material::texture)
        .def_readwrite("normal_texture", &Material::normal_texture);

    py::class_<Mesh>(m, "Mesh")
        .def("vertex_count", [](const Mesh& mesh) { return mesh.vertices.size(); })
        .def("index_count", [](const Mesh& mesh) { return mesh.indices.size(); })
        .def("__repr__", [](const Mesh& mesh) {
            return "<Mesh " + std::to_string(mesh.vertices.size()) + " verts, " +
                   std::to_string(mesh.indices.size()) + " indices>";
        });

    py::class_<Node>(m, "Node")
        .def("drawable", &Node::Drawable)
        .def("__repr__", [](const Node& node) {
            return "<Node mesh=" + std::to_string(node.mesh) +
                   " material=" + std::to_string(node.material) +
                   " parent=" + std::to_string(node.parent) + ">";
        });

    py::class_<Scene>(m, "Scene")
        .def(py::init<>())
        .def_readwrite("camera", &Scene::camera)
        .def_readwrite("ambient", &Scene::ambient)
        .def_readwrite("shadow_map_size", &Scene::shadow_map_size)
        .def_property(
            "background",
            [](const Scene& scene) {
                return std::vector<float>(scene.background.begin(), scene.background.end());
            },
            [](Scene& scene, const std::vector<float>& colour) {
                for (std::size_t index = 0; index < scene.background.size() &&
                                            index < colour.size();
                     ++index) {
                    scene.background[index] = colour[index];
                }
            })
        // Appends rather than the containers themselves. This is not a style choice: a std::vector
        // crosses into Python as a copy, so `scene.meshes.append(...)` is a list that is thrown
        // away, and the scene stays empty without anything saying so.
        .def("add_mesh",
             [](Scene& scene, const Mesh& mesh) {
                 scene.meshes.push_back(mesh);
                 return scene.meshes.size() - 1;
             })
        .def("add_material",
             [](Scene& scene, const Material& material) {
                 scene.materials.push_back(material);
                 return scene.materials.size() - 1;
             })
        .def("add_texture",
             [](Scene& scene, const Texture& texture) {
                 scene.textures.push_back(texture);
                 return scene.textures.size() - 1;
             })
        .def("add_light",
             [](Scene& scene, const Light& light) {
                 scene.lights.push_back(light);
                 return scene.lights.size() - 1;
             })
        .def("add_node", &Scene::AddNode, py::arg("mesh"), py::arg("material"), py::arg("local"),
             py::arg("parent") = zlong::engine::kNoParent)
        // The readout the front end needs for a tree and a panel.
        .def("node_count", [](const Scene& scene) { return scene.nodes.size(); })
        .def("mesh_count", [](const Scene& scene) { return scene.meshes.size(); })
        .def("material_count", [](const Scene& scene) { return scene.materials.size(); })
        .def("texture_count", [](const Scene& scene) { return scene.textures.size(); })
        .def("light_count", [](const Scene& scene) { return scene.lights.size(); })
        .def("node_mesh",
             [](const Scene& scene, std::size_t index) { return scene.nodes.at(index).mesh; })
        .def("node_material",
             [](const Scene& scene, std::size_t index) { return scene.nodes.at(index).material; })
        .def("node_parent",
             [](const Scene& scene, std::size_t index) { return scene.nodes.at(index).parent; })
        .def("node_drawable",
             [](const Scene& scene, std::size_t index) { return scene.nodes.at(index).Drawable(); })
        .def("set_node_local",
             [](Scene& scene, std::size_t index, const zlong::engine::Mat4& local) {
                 scene.nodes.at(index).local = local;
             },
             py::arg("index"), py::arg("local"))
        .def("set_node_material",
             [](Scene& scene, std::size_t index, std::uint32_t material) {
                 scene.nodes.at(index).material = material;
             },
             py::arg("index"), py::arg("material"))
        .def("material_tint",
             [](const Scene& scene, std::size_t index) {
                 const auto& tint = scene.materials.at(index).tint;
                 return std::vector<float>(tint.begin(), tint.end());
             })
        .def("set_material_tint",
             [](Scene& scene, std::size_t index, const std::vector<float>& tint) {
                 auto& material = scene.materials.at(index);
                 for (std::size_t channel = 0;
                      channel < material.tint.size() && channel < tint.size(); ++channel) {
                     material.tint[channel] = tint[channel];
                 }
             },
             py::arg("index"), py::arg("tint"))
        .def("__repr__", [](const Scene& scene) {
            return "<Scene " + std::to_string(scene.nodes.size()) + " node(s), " +
                   std::to_string(scene.meshes.size()) + " mesh(es)>";
        });

    // The procedural makers. These are the engine's own toolkit, and everything a developer builds
    // starts from them until there is a content pipeline to load files.
    m.def("make_box", &zlong::engine::MakeBox);
    m.def("make_grid", &zlong::engine::MakeGrid, py::arg("divisions") = 1,
          py::arg("uv_tiles") = 1.0f);
    m.def("make_cylinder", &zlong::engine::MakeCylinder, py::arg("sides") = 16);
    m.def("make_cone", &zlong::engine::MakeCone, py::arg("sides") = 16);
    m.def("make_disc", &zlong::engine::MakeDisc, py::arg("sides") = 32);
    m.def("make_sphere", &zlong::engine::MakeSphere, py::arg("rings") = 8,
          py::arg("segments") = 16);
    m.def("make_checker_texture", &zlong::engine::MakeCheckerTexture, py::arg("size"),
          py::arg("cells"));
    m.def("make_ripple_normal_texture", &zlong::engine::MakeRippleNormalTexture, py::arg("size"),
          py::arg("cells"));
    m.attr("kNoMesh") = zlong::engine::kNoMesh;
    m.attr("kNoTexture") = zlong::engine::kNoTexture;
    m.attr("kNoParent") = zlong::engine::kNoParent;

    // The identity transform and the ways to build one, so a Python caller can place something
    // without doing matrix maths itself. Mat4 is opaque to Python -- made by these, handed back to
    // `add_node` -- but it composes with `*`, because composing is the whole of what placing
    // something is.
    py::class_<zlong::engine::Mat4>(m, "Mat4")
        .def("__mul__",
             [](const zlong::engine::Mat4& a, const zlong::engine::Mat4& b) { return a * b; },
             py::is_operator())
        .def("__repr__", [](const zlong::engine::Mat4&) { return "<Mat4>"; });
    m.def("identity", &zlong::engine::Identity);
    m.def("translation", &zlong::engine::Translation, py::arg("x"), py::arg("y"), py::arg("z"));
    m.def("rotation_y", &zlong::engine::RotationY, py::arg("radians"));
    m.def("scale", static_cast<zlong::engine::Mat4 (*)(float)>(&zlong::engine::Scale),
          py::arg("factor"));
    m.def("scale_xyz",
          static_cast<zlong::engine::Mat4 (*)(float, float, float)>(&zlong::engine::Scale),
          py::arg("x"), py::arg("y"), py::arg("z"));
    m.def("radians", &zlong::engine::Radians, py::arg("degrees"));

    py::class_<Platform, PyPlatform>(m, "Platform")
        .def(py::init<>())
        .def("pump", &Platform::Pump)
        .def("present", &Platform::Present, py::arg("rgba"), py::arg("pitch"), py::arg("width"),
             py::arg("height"))
        .def("on_frame", &Platform::OnFrame, py::arg("scene"), py::arg("seconds"))
        .def("on_mix", &Platform::OnMix, py::arg("scene"), py::arg("seconds"));

    py::enum_<System::FrameResult>(m, "FrameResult")
        .value("Rendered", System::FrameResult::Rendered)
        .value("Closed", System::FrameResult::Closed)
        .value("Failed", System::FrameResult::Failed);

    py::class_<System> machine(m, "System");

    py::class_<System::Config>(machine, "Config")
        .def(py::init<>())
        .def_readwrite("dram_bytes", &System::Config::dram_bytes)
        .def_readwrite("width", &System::Config::width)
        .def_readwrite("height", &System::Config::height)
        .def_readwrite("arena_bytes", &System::Config::arena_bytes)
        .def_readwrite("vulkan", &System::Config::vulkan)
        .def_readwrite("require_vulkan", &System::Config::require_vulkan)
        .def_readwrite("frame_interval_ms", &System::Config::frame_interval_ms)
        .def_readwrite("audio_block_frames", &System::Config::audio_block_frames);

    machine.def(py::init<const System::Config&>(), py::arg("config"))
        .def("ok", &System::ok)
        .def("error", &System::error)
        .def("note", &System::note)
        .def("backend_name", &System::backend_name)
        .def("set_platform", &System::set_platform, py::arg("platform"),
             py::keep_alive<1, 2>())  // the machine borrows it; keep the Python object alive
        .def("load", &System::Load, py::arg("scene"))
        .def("scene", &System::scene, py::return_value_policy::reference_internal)
        .def("frame", &System::Frame)
        .def("run", &System::Run, py::arg("frames"))
        .def("frames", &System::frames)
        .def("draws", &System::draws)
        .def("faults", &System::faults)
        .def("syncpoint_signals", &System::syncpoint_signals);
}
