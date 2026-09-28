"""What a new file starts as.

Every scene in this project is a program, so the useful thing a template can do is hand over the
shape of one: open a `System`, build a `Scene` out of the engine's procedural toolkit, load it, draw
it. There is no content pipeline yet, so the primitives in the engine are the whole modelling
vocabulary and the scene template shows all of them.

Both faces of the machine are here. The Python one goes through the module; the C++ one goes
through the libraries, which is why it is compiled before it is run.
"""

from __future__ import annotations

EMPTY = ("空脚本", '''"""一个新的脚本。

`import zlong` 拿到的是整个内核 + 引擎 + 宿主：System 是那台机器，Scene 是它的文档。
按 F5 运行，输出会落在下面的控制台里。
"""

import zlong
''')

SCENE = ("场景脚本", '''"""一个最小的场景：建出来，交给机器，画出来。

zlong.System 就是那台机器。Config 决定它多大、是不是用 vulkan；Scene 是文档；
set_platform 之后，机器会把每一帧画好的像素回调给这个 Python 对象。
"""

import zlong

WIDTH, HEIGHT = 1280, 720


class SaveFrame(zlong.Platform):
    """宿主那一半：pump 返回 False 就停，present 拿到颜色缓冲。"""

    def pump(self) -> bool:
        return True

    def present(self, rgba: bytes, pitch: int, width: int, height: int) -> None:
        from PySide6.QtGui import QImage

        QImage(rgba, width, height, pitch, QImage.Format.Format_RGBA8888).copy().save("out.png")


def build() -> zlong.Scene:
    scene = zlong.Scene()
    scene.background = [0.09, 0.10, 0.13]
    scene.ambient = zlong.Vec3(0.24, 0.25, 0.28)
    scene.shadow_map_size = 1024

    scene.camera.eye = zlong.Vec3(5.0, 3.4, 6.6)
    scene.camera.target = zlong.Vec3(0.0, 0.8, 0.0)
    scene.camera.fov_y_radians = zlong.radians(48.0)
    scene.camera.near_z = 0.1
    scene.camera.far_z = 80.0

    sun = zlong.Light()
    sun.direction = zlong.Vec3(0.45, -0.85, 0.30)
    sun.colour = zlong.Vec3(1.0, 0.97, 0.90)
    scene.add_light(sun)

    # 每个图元都是一个单位大：方块是 [-0.5, 0.5]，柱和锥绕原点高一格，球和圆盘是一格直径。
    # 所以「放在地上」就是先缩放、再抬高它的一半。
    floor = zlong.Material()
    floor.tint = [0.52, 0.55, 0.50, 1.0]
    scene.add_node(scene.add_mesh(zlong.make_grid(16, 16.0)), scene.add_material(floor),
                   zlong.scale_xyz(16.0, 1.0, 16.0))

    jade = zlong.Material()
    jade.tint = [0.25, 0.62, 0.48, 1.0]
    jade.specular = 0.5
    scene.add_node(scene.add_mesh(zlong.make_cylinder(24)), scene.add_material(jade),
                   zlong.translation(0.0, 1.5, 0.0) * zlong.scale_xyz(0.7, 3.0, 0.7))
    return scene


def main() -> int:
    config = zlong.System.Config()
    config.width, config.height = WIDTH, HEIGHT
    config.dram_bytes = 512 * 1024 * 1024
    # arena 是机器从 RAM 里划出来的一块，装下所有渲染目标和几何 —— 画面越大，它就要越大。
    config.arena_bytes = 64 * 1024 * 1024
    config.vulkan = True
    config.require_vulkan = False
    config.frame_interval_ms = 0.0

    machine = zlong.System(config)
    if not machine.ok():
        print("机器起不来：", machine.error())
        return 1
    print("backend:", machine.backend_name())

    machine.set_platform(SaveFrame())
    if not machine.load(build()):
        print("load 被拒：", machine.error())
        return 1

    machine.run(3)
    print(f"frames={machine.frames()} draws={machine.draws()} faults={machine.faults()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
''')

HOST = ("宿主脚本", '''"""只讲宿主那一半：机器每帧回调进来的四个入口。

Platform 是宿主实现的那个接口 —— 机器不认识窗口，也不认识声卡，它只认识这四个方法。
把窗口换成写文件、把声音换成算波形，都是同一件事。
"""

import zlong


class MyHost(zlong.Platform):
    def __init__(self) -> None:
        super().__init__()
        self.frames = 0

    def pump(self) -> bool:
        """返回 False 就让机器停下来。"""
        return True

    def on_frame(self, scene: zlong.Scene, seconds: float) -> None:
        """一帧的动画：在这里改 scene，机器接下来就照改完的画。"""
        self.frames += 1

    def present(self, rgba: bytes, pitch: int, width: int, height: int) -> None:
        """画完了的一帧，RGBA8。"""
        if self.frames == 1:
            print(f"first frame: {width}x{height}, {pitch} bytes per row")

    def on_mix(self, scene: zlong.Scene, seconds: float) -> None:
        """一帧的声音，和 on_frame 是各自的时钟。"""
''')

CPP = ("C++ 主机程序", '''// 一个用 C++ 写的宿主程序：和 Python 那份一样，只是走的不是模块，是库本身。
//
// 按 F5：编辑器会用 MSVC 把它编出来，链上项目已经构建好的 zlong_system.lib 和它下面的一切，
// 然后运行。所以这里能 include 的头文件，和 宿主/ 下面那些程序能用的是同一套。
//
// 编译需要项目先构建过一次 —— 编辑器从 build/CMakeCache.txt 里读它该带哪些宏、哪些库。

#include <cstdio>

#include "zlong/system/system.h"

namespace {

/// 宿主那一半。机器不认识窗口，也不认识声卡，它只认识这四个方法。
class MyHost final : public zlong::system::Platform {
public:
    bool Pump() override { return true; }

    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                 std::uint32_t height) override {
        (void)rgba;
        if (frames_ == 0) {
            std::printf("第一帧：%ux%u，每行 %u 字节\\n", width, height, pitch);
        }
        ++frames_;
    }

    void OnFrame(zlong::engine::Scene& scene, double seconds) override {
        (void)scene;
        (void)seconds;
    }

private:
    int frames_ = 0;
};

}  // namespace

int main() {
    zlong::system::System::Config config;
    config.width = 1280;
    config.height = 720;
    // arena 是机器从 RAM 里划出来的一块，装下所有渲染目标和几何 —— 画面越大，它就要越大。
    config.dram_bytes = 512ull * 1024 * 1024;
    config.arena_bytes = 64ull * 1024 * 1024;
    config.vulkan = true;
    config.frame_interval_ms = 0.0;

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::printf("机器起不来：%s\\n", machine.error().c_str());
        return 1;
    }
    std::printf("backend: %s\\n", machine.backend_name());

    zlong::engine::Scene scene;
    scene.background = {0.09f, 0.10f, 0.13f, 1.0f};
    scene.ambient = zlong::engine::Vec3{0.24f, 0.25f, 0.28f};
    scene.shadow_map_size = 1024;
    scene.camera.eye = zlong::engine::Vec3{5.0f, 3.4f, 6.6f};
    scene.camera.target = zlong::engine::Vec3{0.0f, 0.8f, 0.0f};
    scene.camera.fov_y_radians = zlong::engine::Radians(48.0f);

    zlong::engine::Light sun;
    sun.direction = zlong::engine::Vec3{0.45f, -0.85f, 0.30f};
    sun.colour = zlong::engine::Vec3{1.0f, 0.97f, 0.90f};
    scene.lights.push_back(sun);

    // 每个图元都是一个单位大：方块是 [-0.5, 0.5]，柱和锥绕原点高一格。所以「放在地上」就是
    // 先缩放、再抬高它的一半。
    const auto floor_mesh = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(zlong::engine::MakeGrid(16, 16.0f));

    zlong::engine::Material floor;
    floor.tint = {0.52f, 0.55f, 0.50f, 1.0f};
    const auto floor_material = static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(floor);
    scene.AddNode(floor_mesh, floor_material,
                  zlong::engine::Scale(16.0f, 1.0f, 16.0f));

    const auto column_mesh = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(zlong::engine::MakeCylinder(24));

    zlong::engine::Material jade;
    jade.tint = {0.25f, 0.62f, 0.48f, 1.0f};
    jade.specular = 0.5f;
    const auto jade_material = static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(jade);
    scene.AddNode(column_mesh, jade_material,
                  zlong::engine::Translation(0.0f, 1.5f, 0.0f) *
                      zlong::engine::Scale(0.7f, 3.0f, 0.7f));

    MyHost host;
    machine.set_platform(&host);

    if (!machine.Load(scene)) {
        std::printf("load 被拒：%s\\n", machine.error().c_str());
        return 1;
    }

    machine.Run(3);
    std::printf("frames=%zu draws=%zu faults=%zu\\n", machine.frames(), machine.draws(),
                machine.faults());
    return 0;
}
''')
