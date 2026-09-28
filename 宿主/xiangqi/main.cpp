// 烛龙 (ZhuLong) - 象棋 (Chinese chess): the game, on the machine.
//
// A host program, and the fourth one: it describes a game, hands the machine a scene and a
// Platform, and lets the kernel drive. What is here is only what a program can know --
// a window, the keyboard, and how long the last frame took; the rules are in rules.cpp and
// everything on screen is in presenter.cpp.
//
// Vulkan is required, not preferred. The renderer here is a lit board of thirty-two
// figures with a shadow; the software backend draws the same picture correctly but takes
// most of a second a frame, which is fine for a regression test and useless to play.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#pragma warning(push, 0)
#include <windows.h>
#pragma warning(pop)
#endif

#include "../editor_host.h"
#include "../render_scene/bmp.h"
#include "presenter.h"
#include "zlong/system/system.h"

namespace {

constexpr std::uint32_t kWidth = 1024;
constexpr std::uint32_t kHeight = 640;

#if defined(_WIN32)

/// A window and a 32-bit top-down DIB to hand it pixels through, the same one the viewer
/// uses: a buffer this process writes into, so presenting is one StretchDIBits.
class Window {
public:
    ~Window() { Close(); }

    bool Open(const wchar_t* title, std::uint32_t width, std::uint32_t height) {
        width_ = width;
        height_ = height;

        WNDCLASSW klass{};
        klass.lpfnWndProc = &Window::Procedure;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"ZhuLongXiangqi";
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

    /// The presses since the last call, as virtual-key codes, oldest first.
    ///
    /// The window procedure fills this and the frame loop drains it. Polling `GetAsyncKeyState`
    /// instead -- which is what this did -- misses any press that begins and ends between two
    /// frames, and at eleven frames a second most taps do: twelve fifteen-millisecond taps had
    /// the game see *two* of them. That is what "the pieces will not move" turned out to be.
    std::vector<int> TakePresses() {
        std::vector<int> taken;
        taken.swap(presses_);
        return taken;
    }

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
        if (message == WM_NCCREATE) {
            // Keep the instance, so the procedure can reach the press queue.
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        if (message == WM_KEYDOWN) {
            // Bit 30 says the key was already down: that is the auto-repeat, not a press.
            if ((lparam & (1 << 30)) == 0) {
                if (auto* self = reinterpret_cast<Window*>(
                        GetWindowLongPtrW(window, GWLP_USERDATA))) {
                    self->presses_.push_back(static_cast<int>(wparam));
                }
            }
            return 0;
        }
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
    std::vector<int> presses_;
};

#endif  // _WIN32

/// A scripted game, so the picture can be checked without a keyboard: red's cannon takes
/// the black horse down file 1, over the screen at (1,7). That is the capture that is a
/// *shot* rather than a lunge, and the one that exercises the fall as well.
zlong::xiangqi::Keys DemoKeys(int frame) {
    zlong::xiangqi::Keys keys;
    // The cursor starts on the red general at (4,0); the cannon is at (1,2), the horse at
    // (1,9). One move a frame, because a press is what the presenter reads.
    //
    // The file has to go *down* to 1, and that is three presses of the right-hand key: on this
    // board a smaller file is further right, because the camera looks along +Z.
    if (frame >= 2 && frame <= 4) {
        keys.right = true;
    }
    if (frame == 5 || frame == 6) {
        keys.up = true;
    }
    if (frame == 8) {
        keys.confirm = true;
    }
    if (frame >= 10 && frame <= 16) {
        keys.up = true;
    }
    if (frame == 18) {
        keys.confirm = true;
    }
    return keys;
}

/// The platform half: a window, and a keyboard turned into *presses*.
///
/// A chess move is a request, not a rate, so a held key must fire once. The edge is
/// detected here, which is the only place that knows what "held" means.
class GamePlatform final : public zlong::system::Platform {
public:
    GamePlatform() : game_(zlong::xiangqi::Board::Start()) {}

    /// Add every node the game will ever use. Must run before the machine prepares the
    /// scene, because the node count is fixed there.
    void Build(zlong::engine::Scene& scene) { game_.Build(scene); }

    /// Play the scripted game instead of reading a keyboard.
    void set_demo(bool on) noexcept { demo_ = on; }

    /// Hand black to the computer, or take it back.
    void set_computer(bool on) noexcept { game_.set_computer(on); }

    /// Let the computer play both sides, so a whole game happens with nobody at the keys.
    void set_self_play(bool on) noexcept {
        game_.set_self_play(on);
        game_.set_computer(on);
    }

    /// Take the window the editor offers instead of making one of our own.
    ///
    /// Set `ZL_EDITOR_HOST` and the program has no window: frames go to the panel on the right and
    /// the keys typed there come back as the same virtual-key codes the window used to queue, so
    /// the mapping below does not change either way.
    bool OpenWindow() {
#if defined(_WIN32)
        if (zlong::editor::requested()) {
            editor_ = std::make_unique<zlong::editor::Host>();
            if (editor_->ready()) {
                return true;
            }
            editor_.reset();          // asked for but unusable: fall back to a real window
        }
        windowed_ = window_.Open(L"烛龙 - 象棋", kWidth, kHeight);
        return windowed_;
#else
        return true;
#endif
    }

    bool Pump() override {
#if defined(_WIN32)
        if (editor_ != nullptr && !editor_->Pump()) {
            closed_ = true;
        }
        // A headless run has no window to pump and nothing to close.
        if (windowed_ && window_.Pump() == false) {
            closed_ = true;
        }
#endif
        return !closed_;
    }

    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                 std::uint32_t height) override {
#if defined(_WIN32)
        if (editor_ != nullptr) {
            editor_->Present(rgba, pitch, width, height);
            return;
        }
        window_.Present(rgba, pitch);
#else
        (void)rgba;
        (void)pitch;
        (void)width;
        (void)height;
#endif
    }

    void OnFrame(zlong::engine::Scene& scene, double) override {
        // The wall clock, not the render time the machine hands over: an animation should
        // take as long as it says even when a frame is cheap.
        const auto now = std::chrono::steady_clock::now();
        double seconds = 0.0;
        if (started_) {
            seconds = std::chrono::duration<double>(now - last_).count();
        }
        last_ = now;
        started_ = true;

        game_.Update(scene, demo_ ? DemoKeys(frame_) : ReadKeys(), seconds);
        ++frame_;
        const std::string event = game_.TakeEvent();
        if (!event.empty()) {
            std::printf("%s\n", event.c_str());
        }
    }

private:
#if defined(_WIN32)
    zlong::xiangqi::Keys ReadKeys() {
        zlong::xiangqi::Keys keys;
        const auto count = [&keys](int key) {
            switch (key) {
                case VK_LEFT: ++keys.left; break;
                case VK_RIGHT: ++keys.right; break;
                case VK_UP: ++keys.up; break;
                case VK_DOWN: ++keys.down; break;
                case VK_RETURN:
                case VK_SPACE: ++keys.confirm; break;
                case VK_ESCAPE: ++keys.cancel; break;
                case 'R': ++keys.restart; break;
                case 'C': ++keys.toggle_computer; break;
                default: break;
            }
        };
        // 键盘有两个来源：编辑器那根管子，或者它自己的窗口。**管子那边现在连抬起也送了**
        // （修饰键要它），而这个游戏只认按下 —— 按下的那一下才是"按了一下"。
        if (editor_ != nullptr) {
            for (const zlong::editor::Host::Key& key : editor_->TakeKeys()) {
                if (key.down) {
                    count(key.code);
                }
            }
        } else {
            for (const int key : window_.TakePresses()) {
                count(key);
            }
        }
        return keys;
    }

    Window window_;
    std::unique_ptr<zlong::editor::Host> editor_;
    bool closed_ = false;
    bool windowed_ = false;
#else
    zlong::xiangqi::Keys ReadKeys() { return {}; }
#endif

    zlong::xiangqi::Presenter game_;
    std::chrono::steady_clock::time_point last_{};
    bool started_ = false;
    bool demo_ = false;
    int frame_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    int frame_limit = -1;
    bool headless = false;
    bool probe = false;
    bool demo = false;
    bool selfplay = false;
    std::string output;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--frames" && index + 1 < argc) {
            frame_limit = std::atoi(argv[++index]);
        } else if (argument == "--headless") {
            headless = true;
        } else if (argument == "--probe") {
            probe = true;
        } else if (argument == "--demo") {
            demo = true;
            headless = true;
        } else if (argument == "--selfplay") {
            selfplay = true;
            headless = true;
        } else if (argument == "--write" && index + 1 < argc) {
            output = argv[++index];
        }
    }

    zlong::system::System::Config config;
    config.vulkan = true;
    config.require_vulkan = true;  // no software path: this is a game, not a test
    config.width = kWidth;
    config.height = kHeight;
    config.dram_bytes = 256ull * 1024 * 1024;
    config.arena_bytes = 64ull * 1024 * 1024;

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::fprintf(stderr, "the machine could not be built: %s\n", machine.error().c_str());
        return 1;
    }
    if (!machine.note().empty()) {
        std::fprintf(stderr, "%s\n", machine.note().c_str());
    }
    std::printf("backend: %s\n", machine.backend_name());
    std::printf("arrows move the cursor, enter picks and places, esc clears, R starts again,\n");
    std::printf("C hands black to the computer (it plays black unless you take it back)\n");

    GamePlatform platform;
    zlong::engine::Scene scene;
    platform.Build(scene);
    if (!machine.Load(scene)) {
        std::fprintf(stderr, "the board could not be prepared: %s\n", machine.error().c_str());
        return 1;
    }
    std::size_t drawables = 0;
    for (const auto& node : machine.scene().nodes) {
        if (node.Drawable()) {
            ++drawables;
        }
    }
    std::printf("nodes: %zu, drawable: %zu\n", machine.scene().nodes.size(), drawables);

    machine.set_platform(&platform);

    if (demo) {
        platform.set_demo(true);
        platform.set_computer(false);  // the script is the only player in a demo
    }
    if (selfplay) {
        platform.set_self_play(true);
    }

    // Where the board actually lands on screen, computed with the engine's own maths. "The
    // front rank is cut off" is a claim worth being able to answer exactly rather than by
    // eye.
    if (probe) {
        const auto& camera = machine.scene().camera;
        const float aspect = static_cast<float>(machine.renderer().width()) /
                             static_cast<float>(machine.renderer().height());
        const zlong::engine::Mat4 view = camera.View();
        const zlong::engine::Mat4 projection = camera.Projection(aspect);
        // Column convention, which is the one the engine's shaders use: clip = P * (V * p).
        const auto project = [&](zlong::engine::Vec3 point) {
            const float p[4] = {point.x, point.y, point.z, 1.0f};
            float v[4];
            for (int row = 0; row < 4; ++row) {
                v[row] = 0.0f;
                for (int column = 0; column < 4; ++column) {
                    v[row] += view.m[row][column] * p[column];
                }
            }
            float clip[4];
            for (int row = 0; row < 4; ++row) {
                clip[row] = 0.0f;
                for (int column = 0; column < 4; ++column) {
                    clip[row] += projection.m[row][column] * v[column];
                }
            }
            return std::pair<float, float>{clip[0] / clip[3], clip[1] / clip[3]};
        };
        std::printf("camera: eye %.1f,%.1f,%.1f -> %.1f,%.1f,%.1f fov %.1f deg\n", camera.eye.x,
                    camera.eye.y, camera.eye.z, camera.target.x, camera.target.y, camera.target.z,
                    camera.fov_y_radians * 180.0f / 3.14159265f);
        for (const float z : {6.2f, 4.5f, 0.0f, -4.5f, -6.2f}) {
            const auto ndc = project(zlong::engine::Vec3{0.0f, 0.0f, z});
            std::printf("  z=%+5.1f -> screen %.3f (0 at the top, 1 at the bottom)\n", z,
                        (1.0f - ndc.second) * 0.5f);
        }
        for (const auto& square :
             {zlong::xiangqi::Square{4, 0}, zlong::xiangqi::Square{4, 9},
              zlong::xiangqi::Square{0, 0}, zlong::xiangqi::Square{0, 9}}) {
            const auto ndc = project(zlong::xiangqi::SquareCentre(square));
            std::printf("  piece at file %d rank %d -> screen x %.3f y %.3f\n", square.file,
                        square.rank, (1.0f + ndc.first) * 0.5f, (1.0f - ndc.second) * 0.5f);
        }
    }

#if !defined(_WIN32)
    (void)headless;
    std::fprintf(stderr, "no window on this platform\n");
    return 1;
#else
    if (!headless && !platform.OpenWindow()) {
        return 1;
    }
    if (headless && frame_limit < 0) {
        frame_limit = 1;
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();

    const auto write = [&](const std::string& path) {
        if (!host::WriteBmp(path, machine.renderer().colour_pixels(), machine.renderer().width(),
                            machine.renderer().height(), machine.renderer().target_pitch())) {
            std::fprintf(stderr, "could not write %s\n", path.c_str());
            return false;
        }
        std::printf("wrote %s\n", path.c_str());
        return true;
    };

    if (demo) {
        // Stop at the interesting moments rather than at the end: the frame before the
        // shot, the shot in the air, the horse going over, and the board after. Each Run
        // continues from where the last one stopped, so this is one continuous game.
        const std::string base = output.empty() ? std::string("xiangqi_demo.bmp") : output;
        int at = 0;
        int drawn = 0;
        for (const int stop : {17, 25, 33, 45}) {
            const int more = machine.Run(stop - at);
            if (more < 0) {
                std::fprintf(stderr, "a frame could not be drawn: %s\n", machine.error().c_str());
                return 1;
            }
            at += more;
            drawn += more;
            if (!write(base + "." + std::to_string(stop) + ".bmp")) {
                return 1;
            }
        }
        std::printf("demo: %d frame(s) in %.2f s\n", drawn,
                    std::chrono::duration<double>(Clock::now() - started).count());
        return 0;
    }

    if (headless && frame_limit < 0) {
        frame_limit = 1;
    }
    const int drawn = machine.Run(frame_limit);
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    if (drawn < 0) {
        std::fprintf(stderr, "a frame could not be drawn: %s\n", machine.error().c_str());
        return 1;
    }
    std::printf("drew %d frame(s) in %.2f s (%.1f ms/frame), %zu gpu fault(s)\n", drawn, elapsed,
                drawn > 0 ? elapsed * 1000.0 / drawn : 0.0, machine.faults());
    const auto& engine = machine.renderer().profile();
    std::printf("  engine: prepare %.2f + constants %.2f + stream %.2f + submit %.2f ms\n",
                engine.prepare_ms, engine.constants_ms, engine.stream_ms, engine.submit_ms);
    const auto& device = machine.gpu().profile();
    std::printf("  device: decode %.2f + execute %.2f + flush %.2f ms\n", device.decode_ms,
                device.execute_ms, device.flush_ms);
    for (const auto& fault : machine.fault_log()) {
        std::fprintf(stderr, "  gpu fault (kind %d): %s\n", static_cast<int>(fault.kind),
                     fault.detail.c_str());
    }

    if (!output.empty()) {
        return write(output) ? 0 : 1;
    }
    return 0;
#endif
}
