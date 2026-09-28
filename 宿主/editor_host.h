#pragma once
//
// 在编辑器里当窗口用。
//
// 按 F5 跑一个 C++ 程序时，编辑器会设 `ZL_EDITOR_HOST`，然后这个程序就不再需要自己的窗口：
// 帧从 stderr 出去，按键从 stdin 进来，面板就是它显示的地方。没设这个变量的时候，`requested()`
// 返回 false，程序照旧开自己的窗口 —— 所以同一个 host 既能独立跑，也能在编辑器里跑，不需要两份。
//
// **帧**走的就是 Python 脚本那份线（见 编码器/frames.py）：二十字节的头（`ZLFR`、宽、高、pitch、
// 长度）后面跟 RGBA8。所以面板不需要知道画面是谁画的。
//
// 帧**不阻塞**：一帧 1280×720 就是 3.7MB，60fps 就是每秒 220MB，直接同步写的话程序会被编辑器的
// 读取速度拖着走。所以发送在另一个线程上，并且只保留**最新的一帧** —— 编辑器还没取走上一帧时，
// 新的直接把它顶掉。看画面的人要的是"现在是什么样"，不是"刚才每一帧都长什么样"；被顶掉的帧记在
// `dropped()` 里，程序自己可以说出来。
//
// **按键**回来的是虚拟键码 —— 和窗口过程当初塞进队列里的那些整数完全一样。这一点是有意的：
// host 里那张映射表（VK_LEFT 管左移、VK_RETURN 管落子…）因此一个字都不用改。
//
// 这个头不依赖 zlong 的任何东西，只是一个会说话的小工具，所以谁都能 include 它。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// This header pulls in windows.h, so it has to leave the surroundings as it found them: `min` and
// `max` as macros break every `std::min` in the engine's headers that come after it.
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace zlong::editor {

/// 编辑器让我们把画面给它吗？
inline bool requested() {
    const char* const value = std::getenv("ZL_EDITOR_HOST");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

/// 一帧一槽，另一根线程往外搬。
///
/// 生产者不等消费者：槽里已经有东西了就把它丢掉、放上新的。这样帧循环的时间只花在游戏上，
/// 花多少时间显示是编辑器的事。
///
/// 搬运线程**不 join**。写管道是阻塞的，而读那头可能已经不读了（编辑器被杀掉、或者只是没在
/// 转事件循环），那样 join 会让游戏退出不了 —— 一个看画面的人把游戏卡死，是绝对不能有的。
/// 所以线程拿着 state 的 shared_ptr 自己跑，`finish()` 只负责让它知道该收了。
class Sender {
public:
    explicit Sender(std::FILE* out) {
        if (out == nullptr) {
            return;
        }
        state_->out = out;
        auto state = state_;
        std::thread([state] { write_forever(state); }).detach();
    }

    /// 把一帧摆上去。槽被占着就顶掉旧的 —— 那正是丢帧。
    void offer(std::vector<unsigned char> packet) {
        if (state_->out == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->packet.empty()) {
                ++state_->dropped;             // 编辑器还堵在上一帧上，这一帧不值得排它后面
            }
            state_->packet = std::move(packet);
        }
        state_->ready.notify_one();
    }

    /// 已经交给编辑器的帧数（不算还在槽里和已经丢掉的）。
    unsigned long long sent() const { return state_->sent.load(); }

    /// 因为编辑器跟不上而没送出去的帧数。
    unsigned long long dropped() const { return state_->dropped.load(); }

    /// 让搬运线程收工。
    void finish() {
        if (state_->out == nullptr || state_->stopping) {
            return;
        }
        // 给手里那一帧一点时间出去，但不无限等：等在写一根没人读的管子上的话，游戏就退不出了。
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->packet.empty()) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopping = true;
        }
        state_->ready.notify_all();
    }

private:
    struct State {
        std::FILE* out = nullptr;
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<unsigned char> packet;
        bool stopping = false;
        std::atomic<unsigned long long> sent{0};
        std::atomic<unsigned long long> dropped{0};
    };

    static void write_forever(std::shared_ptr<State> state) {
        for (;;) {
            std::vector<unsigned char> packet;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->ready.wait(lock, [&] {
                    return !state->packet.empty() || state->stopping;
                });
                if (state->packet.empty()) {
                    return;                    // stopping, and nothing left to send
                }
                packet = std::move(state->packet);
                state->packet.clear();
            }
            if (std::fwrite(packet.data(), 1, packet.size(), state->out) != packet.size()) {
                return;                        // 编辑器没了，写不进去了
            }
            std::fflush(state->out);
            ++state->sent;
        }
    }

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

/// 编辑器，披着一张窗口的皮。
class Host {
public:
    Host() {
#if defined(_WIN32)
        // stderr 变成画面通道，那么原本要去 stderr 的东西（包括 C++ 运行库自己的抱怨）就得
        // 改道去 stdout —— 控制台在那儿。
        const int taken = _dup(_fileno(stderr));
        if (taken >= 0) {
            // 这一步不能省。`_fdopen` 带你给的模式建流，但**不改 fd 本身的模式**，而 stderr 默认是
            // 文本模式：那样数据里每一个 0x0A 都会写成 0x0D 0x0A，帧就烂了。而且烂得挑时候 ——
            // 8×8 的帧头正好不含 0x0A，看着一切正常；512×320 的 length 字段是 0x000A0000，就中了。
            _setmode(taken, _O_BINARY);
            frames_ = _fdopen(taken, "wb");
        }
        _dup2(_fileno(stdout), _fileno(stderr));
        input_ = ::GetStdHandle(STD_INPUT_HANDLE);
#endif
        sender_ = std::make_unique<Sender>(frames_);
    }

    ~Host() {
        if (sender_ != nullptr) {
            sender_->finish();
        }
    }

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// 拿到了画面通道没有。没有的话（没设 `ZL_EDITOR_HOST`、或者 fd 用不了）就别用它。
    bool ready() const { return frames_ != nullptr; }

    /// 已经显示出来的帧数。
    unsigned long long frames_shown() const { return sender_ ? sender_->sent() : 0; }

    /// 因为编辑器跟不上而丢掉的帧数。
    unsigned long long frames_dropped() const { return sender_ ? sender_->dropped() : 0; }

    /// 把这一阵子敲进来的键收下来。编辑器走了就返回 false —— 和窗口关掉是同一件事。
    ///
    /// 一次也不阻塞：管道里有多少读多少，所以帧循环不会被没有按键的空管子卡住。
    bool Pump() {
#if defined(_WIN32)
        if (input_ == nullptr) {
            return false;
        }
        for (;;) {
            DWORD waiting = 0;
            if (::PeekNamedPipe(input_, nullptr, 0, nullptr, &waiting, nullptr) == 0) {
                // 写的那头没了：编辑器把这个程序停了。
                return false;
            }
            if (waiting == 0) {
                break;
            }
            char block[512];
            const DWORD want =
                waiting < sizeof(block) ? waiting : static_cast<DWORD>(sizeof(block));
            DWORD got = 0;
            if (::ReadFile(input_, block, want, &got, nullptr) == 0 || got == 0) {
                break;
            }
            pending_.append(block, got);
        }
        take_lines();
#endif
        return true;
    }

    /// 把一帧交给编辑器。参数就是 `Platform::Present` 收到的那几个。
    ///
    /// 立刻返回：真正写出去的是搬运线程。
    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                 std::uint32_t height) {
        if (frames_ == nullptr || sender_ == nullptr || rgba == nullptr || width == 0 ||
            height == 0) {
            return;
        }
        const std::uint32_t length = pitch * height;
        std::vector<unsigned char> packet(20 + length);
        std::memcpy(packet.data(), "ZLFR", 4);
        put(packet.data() + 4, width);
        put(packet.data() + 8, height);
        put(packet.data() + 12, pitch);
        put(packet.data() + 16, length);
        std::memcpy(packet.data() + 20, rgba, length);
        sender_->offer(std::move(packet));
    }

    /// 一次按键。
    struct Key {
        /// 虚拟键码。
        int code = 0;
        /// 按下还是抬起。
        ///
        /// **抬起也要送。** 以前只送按下，那是因为"按一下"和"按着不放"在这里没区别；但修饰键
        /// 有区别 —— 按着 Shift 拖着走和松开 Shift 拖着走是两件事，而"它松了"只有抬起那一下
        /// 说得出来。
        bool down = false;
        /// 那一刻按着的修饰键，按位：**1 Shift、2 Ctrl、4 Alt**（和鼠标那三个键一路排下来）。
        unsigned modifiers = 0;
    };

    /// 上次调用之后的按键，最早的在前。按下和抬起都在里面。
    std::vector<Key> TakeKeys() {
        std::vector<Key> taken;
        taken.swap(presses_);
        return taken;
    }

    // --- 指针 -------------------------------------------------------------------------------
    //
    // 面板里鼠标的位置和动作。**坐标系是画面自己的像素**，不是面板的像素 —— 编辑器已经把
    // 缩放和居中算掉了才发下来，因为只有它知道画面是怎么塞进面板的，而这里只想要一个能变成
    // 射线的坐标。见 `编码器/viewport.py` 的 `frame_at`。

    /// 指针在画面里的位置，上一次 `Pump` 时的。
    struct Pointer {
        int x = 0;
        int y = 0;
    };

    /// **最新的一条赢**，不像按键那样排队：一次拖动会产生几百个移动，而只有最后一个描述
    /// "现在"。排队的话，程序慢下来时鼠标会拖出一条越来越长的尾巴。
    Pointer pointer() const { return pointer_; }

    /// 一个键的按下或抬起。
    struct Click {
        /// 1 左、2 右、4 中 —— 就是 Windows 那套掩码，省一次翻译。
        int button = 0;
        bool down = false;
        /// **动作发生的位置，画面自己的像素。**
        ///
        /// 位置带在事件里，而不是让程序去问"指针现在在哪"，因为两件事不一样：指针是状态，只留
        /// 最新的，而这一下按在哪是一个已经发生的事实。一次按下如果和一次移动落在同一帧里，
        /// 去问指针拿到的是**移动之后**的位置 —— 拖动就少算了那一段，落点比手短一截。
        int x = 0;
        int y = 0;
    };

    /// 上次调用之后的按键动作，最早的在前。
    ///
    /// **这个排队，不像指针那样只留最新的**：一次在两帧之间完成又结束的点击仍然是一次点击，
    /// 丢掉它会让工具感觉"漏了输入"。位置是状态，按下抬起是事件，两者语义不同。
    std::vector<Click> TakeClicks() {
        std::vector<Click> taken;
        taken.swap(clicks_);
        return taken;
    }

    /// 上次调用之后的滚轮格数，向外为正。累加，取走之后清零。
    int TakeWheel() {
        const int taken = wheel_;
        wheel_ = 0;
        return taken;
    }

    /// 一次"给选中的面上这个颜色"。四个通道都是 0..1。
    struct Colour {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    /// 编辑器交下来的颜色，最早的在前。
    std::vector<Colour> TakeColours() {
        std::vector<Colour> taken;
        taken.swap(colours_);
        return taken;
    }

    /// 一次"存 / 读 / 导入"的吩咐。
    struct FileOrder {
        enum class Kind {
            Save,      // 把手上这份存到那儿
            Load,      // 从那儿读一份自己的模型
            Import,    // 从那儿读一份别人的网格文件
            Texture,   // 给选中的面上那儿的一张图
        };
        Kind kind = Kind::Load;
        std::string path;
    };

    /// 编辑器交下来的存取吩咐，最早的在前。
    ///
    /// **路径走管子，不走命令行** —— 这不是偏好，是 Windows 的样子：它把命令行按 ANSI 代码页交给
    /// C 运行库，`烛龙\我的模型.model` 还没进 `main` 就已经不是原来那串字节了（量过，见
    /// `验收模型文件.cpp`）。stdin 上这串是 UTF-8 的字节，原样到得了，所以"存到哪儿"这件事只能
    /// 从这儿说。
    std::vector<FileOrder> TakeFiles() {
        std::vector<FileOrder> taken;
        taken.swap(files_);
        return taken;
    }

private:
    static void put(unsigned char* at, std::uint32_t value) {
        std::memcpy(at, &value, 4);   // 小端，和 编码器/frames.py 里的 `<IIII` 对上
    }

    void take_lines() {
        for (;;) {
            const std::size_t end = pending_.find('\n');
            if (end == std::string::npos) {
                return;
            }
            const std::string line = pending_.substr(0, end);
            pending_.erase(0, end + 1);
            consume(line);
        }
    }

    /// 编辑器往 stdin 上写的那些行。一行一条，第一个字母是类型：
    ///
    ///     K <虚拟键码> <1 按下 / 0 抬起> [修饰键]   键盘；修饰键 1 Shift / 2 Ctrl / 4 Alt
    ///     M <x> <y>                               指针移动，画面自己的像素
    ///     C <1|2|4> <1 按下 / 0 抬起> <x> <y>     左右中键的按下抬起，带上它在哪
    ///     W <格数>                                滚轮，带符号
    ///     A <R> <G> <B> <A>                       给选中的面上这个颜色，四个通道 0..1
    ///     S <路径>                                存到这个路径
    ///     L <路径>                                从这个路径读自己的模型
    ///     I <路径>                                从这个路径导入别人的网格文件
    ///     T <路径>                                给选中的面挂上这个路径的一张图
    ///
    /// 最后四条（带路径的那些）是**行里剩下的全部**，不做分词 —— 路径里有空格，而且它是 UTF-8，
    /// 按空白切会把一条路径切成两半。`K` 那一行的修饰键是**后加的**，老的一行只有两个数，缺了
    /// 就是 0。
    ///
    /// 另一头是 `编码器/main.py` 的 `_on_view_*`，改这里就要改那里 —— 这两处是一套协议。
    /// 认不出来的行**丢掉**，不是报错：管子是共享的，将来多一种消息不该让旧的一端炸掉。
    ///
    /// **这一批里两队的先后是固定的：先键盘，再鼠标，再滚轮。** 同一帧里同时到达的一条键和一次
    /// 点击，键一律算在前面 —— 无论谁先写进来。一帧是十几毫秒，人手做不到故意卡在中间，所以
    /// 不为此另立一套带顺序的事件流；但知道有这回事，才不会指望它按写入顺序发生。
    void consume(const std::string& line) {
        if (line.empty()) {
            return;
        }
        switch (line[0]) {
        case 'K': {
            int key = 0;
            int down = 0;
            unsigned modifiers = 0;
            // 修饰键是后加的：老的一行只有两个数，`sscanf` 收两个就不再往下读，`modifiers` 留着 0。
            if (std::sscanf(line.c_str() + 1, "%d %d %u", &key, &down, &modifiers) >= 2 && key > 0) {
                presses_.push_back(Key{key, down != 0, modifiers});
            }
            break;
        }
        case 'M': {
            int x = 0;
            int y = 0;
            if (std::sscanf(line.c_str() + 1, "%d %d", &x, &y) == 2) {
                pointer_.x = x;
                pointer_.y = y;
            }
            break;
        }
        case 'C': {
            int button = 0;
            int down = 0;
            int x = pointer_.x;
            int y = pointer_.y;
            // 老的一行只有两个数（没有位置）。位置缺了就用指针当前的值 —— 比丢掉这次点击好。
            const int read = std::sscanf(line.c_str() + 1, "%d %d %d %d", &button, &down, &x, &y);
            if (read >= 2 && button > 0) {
                clicks_.push_back(Click{button, down != 0, x, y});
            }
            break;
        }
        case 'W': {
            int notches = 0;
            if (std::sscanf(line.c_str() + 1, "%d", &notches) == 1) {
                wheel_ += notches;
            }
            break;
        }
        case 'A': {
            Colour colour;
            if (std::sscanf(line.c_str() + 1, "%f %f %f %f", &colour.r, &colour.g, &colour.b,
                            &colour.a) == 4) {
                colours_.push_back(colour);
            }
            break;
        }
        case 'S':
        case 'L':
        case 'I':
        case 'T': {
            std::size_t at = 1;
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
                ++at;
            }
            if (at < line.size()) {
                const auto kind = line[0] == 'S'   ? FileOrder::Kind::Save
                                  : line[0] == 'L' ? FileOrder::Kind::Load
                                  : line[0] == 'I' ? FileOrder::Kind::Import
                                                   : FileOrder::Kind::Texture;
                files_.push_back(FileOrder{kind, line.substr(at)});
            }
            break;
        }
        default:
            break;
        }
    }

#if defined(_WIN32)
    std::FILE* frames_ = nullptr;
    HANDLE input_ = nullptr;
#endif
    std::unique_ptr<Sender> sender_;
    std::string pending_;
    std::vector<Key> presses_;
    Pointer pointer_;
    std::vector<Click> clicks_;
    int wheel_ = 0;
    std::vector<FileOrder> files_;
    std::vector<Colour> colours_;
};

}  // namespace zlong::editor
