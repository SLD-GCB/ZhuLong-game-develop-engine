// 烛龙 (ZhuLong) - a window, a frame loop, and a camera you can move.
//
// The machine is 内核/system and the loop is its (see System::Run); this program is the
// platform half only. It opens a window, hands it over as a Platform, and answers two
// questions -- where the mouse and keyboard are, and where the pixels go. The engine
// rendering into guest memory and the window that shows it are on opposite sides of the
// seam the kernel drives.
//
// A window made of GDI is the honest choice while the renderer itself is the CPU one: a
// software renderer presenting through a software path, with no new dependency. A
// Vulkan swapchain is the eventual answer and replaces the Present call without the
// machine or the engine changing.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#pragma warning(push, 0)
#include <windows.h>
#pragma warning(pop)
#endif

#include "zlong/engine/scene_file.h"
#include "zlong/system/system.h"

namespace {

using zlong::engine::Light;
using zlong::engine::Scale;
using zlong::engine::Scene;
using zlong::engine::Translation;
using zlong::engine::Vec3;

constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 480;

constexpr std::uint32_t kCheckerTexels = 64;
constexpr std::uint32_t kCheckerCells = 8;
constexpr std::uint32_t kRippleCells = 4;
constexpr std::uint32_t kShadowMapSize = 512;
constexpr int kGridSize = 8;  // kGridSize^2 boxes, plus the ground

/// A ground plane and a square field of boxes of varying height, so the window has
/// something with depth in it to look at from different angles.
Scene MakeScene() {
    Scene scene;
    scene.camera.eye = Vec3{0.0f, 6.0f, 14.0f};
    scene.camera.target = Vec3{0.0f, 1.0f, 0.0f};
    scene.background = {0.05f, 0.06f, 0.10f, 1.0f};

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

    scene.meshes.push_back(zlong::engine::MakeGrid(1));
    scene.meshes.push_back(zlong::engine::MakeBox());
    scene.textures.push_back(zlong::engine::MakeCheckerTexture(kCheckerTexels, kCheckerCells));
    scene.textures.push_back(zlong::engine::MakeRippleNormalTexture(kCheckerTexels, kRippleCells));

    zlong::engine::Material ground;
    ground.texture = 0;
    ground.normal_texture = 1;
    ground.specular = 0.05f;
    scene.materials.push_back(ground);
    // A ramp of colours, so the field reads as depth rather than a wall.
    for (int step = 0; step < 6; ++step) {
        const float t = static_cast<float>(step) / 5.0f;
        zlong::engine::Material material;
        material.tint = {0.25f + 0.65f * t, 0.55f - 0.25f * t, 0.30f + 0.55f * t, 1.0f};
        material.specular = 0.45f;
        scene.materials.push_back(material);
    }

    scene.AddNode(0, 0, Scale(30.0f));

    // Every box hangs off one group node, so the whole field moves or turns by editing a
    // single node instead of sixty-four.
    const std::int32_t field = scene.AddNode(zlong::engine::kNoMesh, 0, zlong::engine::Identity());

    const float spacing = 2.2f;
    const float half = (static_cast<float>(kGridSize) - 1.0f) * 0.5f;
    for (int ix = 0; ix < kGridSize; ++ix) {
        for (int iz = 0; iz < kGridSize; ++iz) {
            // A deterministic "skyline": taller toward the middle, and never zero, so
            // every box sits on the ground.
            const float dx = static_cast<float>(ix) - half;
            const float dz = static_cast<float>(iz) - half;
            const float falloff = 1.0f - std::min(1.0f, (dx * dx + dz * dz) / (half * half + 1.0f));
            const float height = 0.6f + 4.0f * falloff;
            const auto material = static_cast<std::uint32_t>(1 + (ix * 3 + iz * 5) % 6);
            scene.AddNode(1, material,
                          Translation(dx * spacing, height * 0.5f, dz * spacing) *
                              zlong::engine::RotationY(static_cast<float>(ix * 7 + iz * 11) * 0.4f) *
                              Scale(height),
                          field);
        }
    }
    return scene;
}

#if defined(_WIN32)

/// A window and a 32-bit top-down DIB to hand it pixels through. The DIB is what makes
/// this cheap: it is a buffer this process can write into, so presenting is one
/// StretchDIBits and no allocation per frame.
class Window {
public:
    ~Window() { Close(); }

    bool Open(const wchar_t* title, std::uint32_t width, std::uint32_t height) {
        width_ = width;
        height_ = height;

        WNDCLASSW klass{};
        klass.lpfnWndProc = &Window::Procedure;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"ZhuLongViewer";
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (RegisterClassW(&klass) == 0) {
            std::fprintf(stderr, "could not register the window class\n");
            return false;
        }

        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        handle_ = CreateWindowExW(0, klass.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                  nullptr, nullptr, klass.hInstance, this);
        if (handle_ == nullptr) {
            std::fprintf(stderr, "could not create the window\n");
            return false;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);  // top row first
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        info_ = info;

        HDC screen = GetDC(nullptr);
        memory_dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        bitmap_ = CreateDIBSection(memory_dc_, &info_, DIB_RGB_COLORS,
                                   reinterpret_cast<void**>(&bits_), nullptr, 0);
        if (memory_dc_ == nullptr || bitmap_ == nullptr || bits_ == nullptr) {
            std::fprintf(stderr, "could not make a drawing surface\n");
            return false;
        }
        SelectObject(memory_dc_, bitmap_);

        ShowWindow(handle_, SW_SHOW);
        UpdateWindow(handle_);
        return true;
    }

    /// Pump messages. False once the window is gone.
    bool Pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return handle_ != nullptr && IsWindowVisible(handle_) != 0;
    }

    /// RGBA8, top row first, in the machine's byte order.
    void Present(const std::uint8_t* rgba, std::uint32_t pitch) {
        if (bits_ == nullptr || rgba == nullptr) {
            return;
        }
        // DIB wants BGRA in memory, and the target holds RGBA.
        for (std::uint32_t row = 0; row < height_; ++row) {
            const std::uint8_t* source = rgba + static_cast<std::size_t>(row) * pitch;
            std::uint8_t* destination = bits_ + static_cast<std::size_t>(row) * width_ * 4;
            for (std::uint32_t column = 0; column < width_; ++column) {
                destination[column * 4 + 0] = source[column * 4 + 2];
                destination[column * 4 + 1] = source[column * 4 + 1];
                destination[column * 4 + 2] = source[column * 4 + 0];
                destination[column * 4 + 3] = 255;
            }
        }

        HDC dc = GetDC(handle_);
        StretchDIBits(dc, 0, 0, static_cast<int>(width_), static_cast<int>(height_), 0, 0,
                      static_cast<int>(width_), static_cast<int>(height_), bits_, &info_,
                      DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(handle_, dc);
    }

    void Close() {
        if (handle_ != nullptr) {
            DestroyWindow(handle_);
            handle_ = nullptr;
        }
        if (memory_dc_ != nullptr) {
            DeleteDC(memory_dc_);
            memory_dc_ = nullptr;
        }
        if (bitmap_ != nullptr) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        bits_ = nullptr;
    }

private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HWND handle_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    std::uint8_t* bits_ = nullptr;
    BITMAPINFO info_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

bool KeyDown(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

/// The platform half: the window, and the keyboard that orbits the camera. The camera
/// moves in OnFrame, which the machine calls before it draws -- so the frame that goes
/// on screen already has this input in it.
class ViewerPlatform final : public zlong::system::Platform {
public:
    bool Open() { return window_.Open(L"烛龙 - viewer", kWidth, kHeight); }

    bool Pump() override { return !closed_ && window_.Pump(); }

    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t, std::uint32_t) override {
        window_.Present(rgba, pitch);
    }

    void OnFrame(Scene& scene, double) override {
        const bool shift = KeyDown(VK_SHIFT);
        const float speed = shift ? 0.12f : 0.04f;
        if (KeyDown('A') || KeyDown(VK_LEFT)) {
            orbit_ -= speed * 6.0f;
        }
        if (KeyDown('D') || KeyDown(VK_RIGHT)) {
            orbit_ += speed * 6.0f;
        }
        if (KeyDown('W') || KeyDown(VK_UP)) {
            distance_ -= speed * 20.0f;
        }
        if (KeyDown('S') || KeyDown(VK_DOWN)) {
            distance_ += speed * 20.0f;
        }
        if (KeyDown('Q')) {
            elevation_ += speed * 6.0f;
        }
        if (KeyDown('E')) {
            elevation_ -= speed * 6.0f;
        }
        if (KeyDown(VK_ESCAPE)) {
            closed_ = true;
            return;
        }
        distance_ = std::max(2.0f, std::min(80.0f, distance_));

        // The camera orbits a fixed point: enough to inspect a scene from any side
        // without needing mouse capture.
        const Vec3 target = scene.camera.target;
        const float base = 6.0f;
        Vec3 eye;
        eye.x = target.x + distance_ * std::cos(orbit_) * std::cos(base + elevation_);
        eye.y = target.y + distance_ * std::sin(base + elevation_);
        eye.z = target.z + distance_ * std::sin(orbit_) * std::cos(base + elevation_);
        eye.y = std::max(0.3f, eye.y);
        scene.camera.eye = eye;
    }

private:
    Window window_;
    bool closed_ = false;
    float orbit_ = 0.0f;
    float elevation_ = 0.0f;
    float distance_ = 16.0f;
};

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    // --frames N renders N frames and exits, which is how this is checked without a
    // person watching it.
    int frame_limit = -1;
    bool headless = false;
    std::string scene_path;
    zlong::system::System::Config config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--frames" && index + 1 < argc) {
            frame_limit = std::atoi(argv[++index]);
        } else if (argument == "--headless") {
            headless = true;
        } else if (argument == "--vulkan") {
            config.vulkan = true;
        } else if (argument == "--scene" && index + 1 < argc) {
            scene_path = argv[++index];
        }
    }
    config.width = kWidth;
    config.height = kHeight;

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::fprintf(stderr, "the machine could not be built: %s\n", machine.error().c_str());
        return 1;
    }
    if (!machine.note().empty()) {
        std::fprintf(stderr, "%s\n", machine.note().c_str());
    }
    std::printf("backend: %s\n", machine.backend_name());

    std::string error;
    Scene scene;
    if (scene_path.empty()) {
        scene = MakeScene();
    } else if (!zlong::engine::LoadScene(scene_path, scene, error)) {
        std::fprintf(stderr, "the scene %s could not be loaded: %s\n", scene_path.c_str(),
                     error.c_str());
        return 1;
    }
    std::size_t drawables = 0;
    for (const auto& node : scene.nodes) {
        if (node.Drawable()) {
            ++drawables;
        }
    }
    if (!machine.Load(scene)) {
        std::fprintf(stderr, "the scene could not be prepared: %s\n", machine.error().c_str());
        return 1;
    }
    std::printf("scene: %zu node(s), %zu drawable(s), %zu mesh(es)\n", machine.scene().nodes.size(),
                drawables, machine.scene().meshes.size());

#if !defined(_WIN32)
    (void)headless;
    (void)frame_limit;
    std::printf("no window on this platform; nothing to show\n");
    return 0;
#else
    std::unique_ptr<ViewerPlatform> platform;
    if (!headless) {
        platform = std::make_unique<ViewerPlatform>();
        if (!platform->Open()) {
            return 1;
        }
        machine.set_platform(platform.get());
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const int drawn = machine.Run(frame_limit);
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    if (drawn < 0) {
        std::fprintf(stderr, "a frame could not be rendered: %s\n", machine.error().c_str());
        return 1;
    }

    const double ms_per_frame = drawn > 0 ? elapsed * 1000.0 / drawn : 0.0;
    std::printf("rendered %d frame(s), %zu draw(s) each, %.1f ms/frame, %zu gpu fault(s)\n", drawn,
                drawables, ms_per_frame, machine.faults());
    for (const auto& fault : machine.fault_log()) {
        std::fprintf(stderr, "  gpu fault (kind %d): %s\n", static_cast<int>(fault.kind),
                     fault.detail.c_str());
    }

    // What the last frame's time was made of. Printed rather than guessed at: "the
    // generation is expensive" is a claim, and this is the measurement.
    const auto& engine = machine.renderer().profile();
    std::printf("  engine: prepare %.2f + constants %.2f + stream %.2f + submit %.2f ms\n",
                engine.prepare_ms, engine.constants_ms, engine.stream_ms, engine.submit_ms);
    const auto& device = machine.gpu().profile();
    std::printf("  device: decode %.2f + execute %.2f + flush %.2f ms\n", device.decode_ms,
                device.execute_ms, device.flush_ms);
    return 0;
#endif
}
