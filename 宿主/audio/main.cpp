// 烛龙 (ZhuLong) - 音频器：一个宿主程序，把一段声音摆到世界里，**边看边听边改**。
//
// 它和建模器是同一个形状的宿主：实现 `Platform`，交给机器驱动（`System::Run` —— 循环不是宿主的），
// 画面走 stderr、输入从 stdin 来（`../editor_host.h`），一样 `require_vulkan`（视口就是引擎自己的
// 渲染器）。区别只有一处：它不只是"看"，它**出声**。
//
// 世界里的东西：
//
//   * **听者就是相机** —— 这是引擎本来就定下的法（`SoundRenderer` 拿 `scene.camera` 当耳朵）。
//     所以转视角就是转头：左右一转，声像跟着走。这一条不是这里实现的，是它如实照着的那条定律。
//   * **一个声源一个球**。球越大声音越响（增益直接画成大小），选中那个是暖色的，静音那个是灰的。
//     左键点一个选中它，按住左键拖着它在世界里挪 —— 挪到哪边就从哪边听得到，走远了就轻下去
//     （等功率声像 + 线性衰减，两条都是引擎里的）。
//
// 声音从哪来：
//
//   * 面板上「音调」「噪声」「音效」点一下，拿到手里，再在画面上左键放一个（和建模器的图元一个走法，
//     `P`）；「音效」是一段**合成**出来的东西 —— 波形 + 音高走向 + 幅度包络 + 一个低通，见
//     `引擎/include/zlong/engine/synth.h`，改一个参数就照配方重渲染一遍；
//   * 「导入…」读一个 WAV（`wav_file.h`）放进世界（`I`）；
//   * 「导入音乐…」读一份 `.song`（一首曲子：音符过时间，`song.h` / `song_file.h`）放进世界（`U`）
//     —— **默认循环、默认全局**（会跟着位置跑的背景音乐不是背景音乐，见 `Emitter::spatial`）；
//     `G` 或者「全局」把它翻过来；
//   * 「打开…」读一个 `.scene`（复用 `scene_file.h`，读的是它的声音那一半）。
//
// 声音去哪：
//
//   * **实时**：每帧按真实流逝的时间混一小块交给声卡，声卡就是时钟（见 `OnFrame`）。
//   * 「导出 WAV」把当前这一份整体混成一个文件（`E`），**不打扰正在响的那一路** —— 用的是另起一份
//     `SoundRenderer`，不是机器的那个。
//   * 「保存」把这一份写成 `.scene`（`S`；声音/发射器的写出是这一轮新加的）。
//
// 三处顺序值得留意：
//
//   * **只有在编辑的时候才重新 Load。** 相机是每帧的数据，改了不用重传；而 `SoundRenderer::Prepare`
//     会把每个 voice 的游标归零 —— 每帧 Load 会把声音一直从头重启，所以实时播放和重新 Load 是
//     互斥的，编辑才 Load。
//   * **混音量按墙钟算，不按 `OnFrame` 给的那个数。** `last_frame_seconds` 是**渲染耗时**，不是帧
//     间隔（见 `内核/system/src/system.cpp` 的 `Frame`），拿它当混音长度会一直少混。
//   * **拖动从按下那一刻算起，不在上一次结果上累加**（和建模器一个规矩）。

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "../editor_host.h"
#include "zlong/audio/null_sink.h"
#include "zlong/audio/wav_sink.h"
#include "zlong/engine/scene_file.h"
#include "zlong/engine/song.h"
#include "zlong/engine/song_file.h"
#include "zlong/engine/sound.h"
#include "zlong/engine/synth.h"
#include "zlong/engine/wav_file.h"
#include "zlong/system/system.h"

#if defined(_WIN32)
#include "wasapi_sink.h"
#endif

namespace {

using zlong::audio::Clip;
using zlong::audio::Frame;
using zlong::engine::Emitter;
using zlong::engine::Instrument;
using zlong::engine::Light;
using zlong::engine::MakeGrid;
using zlong::engine::MakeSphere;
using zlong::engine::Material;
using zlong::engine::Mat4;
using zlong::engine::Note;
using zlong::engine::Scene;
using zlong::engine::SongSpec;
using zlong::engine::SynthSpec;
using zlong::engine::Translation;
using zlong::engine::Vec3;
using zlong::engine::kNoSound;

// 面板发下来的键号，和 `editor_host.h` 里那套一致。
constexpr int kLeftButton = 1;
constexpr int kRightButton = 2;
constexpr int kMiddleButton = 4;

// 虚拟键码。
constexpr int kSpaceKey = 0x20;
constexpr int kEscapeKey = 0x1B;
constexpr int kUpKey = 0x26;
constexpr int kDownKey = 0x28;
constexpr int kKeyX = 88;   // 删
constexpr int kKeyL = 76;   // 循环
constexpr int kKeyM = 77;   // 静音
constexpr int kKeyG = 71;   // 全局 / 定位（跟不跟位置走）
constexpr int kKeyZ = 90;   // 撤销
constexpr int kKey1 = 49;   // 手里拿上「音调」
constexpr int kKey2 = 50;   // 手里拿上「噪声」
constexpr int kKey3 = 51;   // 手里拿上「音效」

// 相机离目标多近多远就不让再走了，以及俯仰的上下限（越过地平线画面会扭）。
constexpr float kNearest = 1.0f;
constexpr float kFarthest = 80.0f;
constexpr float kPitchLimit = 1.45f;

// 挑声源的宽容度，按**画面像素**算。离得远的球看起来就小，用世界距离量的话，近处点不中、远处一碰
// 一大片（和建模器挑顶点同一个理由）。
constexpr float kPickRadius = 16.0f;

/// 每帧最多补多少秒的混音。一次卡顿（拖动、重新 Load）之后不该把一秒钟的声音一口气灌进去。
constexpr double kMaxMixSeconds = 0.25;

/// 试听那个音的**音长**能调到哪儿（秒）。太短听不出包络，太长了一件音色吊在那里没完。
constexpr float kAuditionShortest = 0.05f;
constexpr float kAuditionLongest = 8.0f;
/// 试听一次最多几步（一个音或一个休止算一步）。每一步都是整份样本，太多了这一段会很长。
constexpr std::size_t kAuditionMaxSteps = 32;

// 撤销栈最多留多少步。每步是整串声源一份快照，而每个声源的 PCM 是**共享指针**，所以一份快照很小。
constexpr std::size_t kUndoDepth = 64;

// 回给编辑器的**声源表**那些行带这个前缀（走 stdout，和文字同一条管子）。和建模器用的是同一个哨兵，
/// 编辑器按它分流到大纲视图（`runtime.py` 的 `SCENE_PREFIX`）。
constexpr const char* kSceneMark = "\x01" "zl ";

/// 手里拿着的东西：没有，或一种要放下的声源。
enum class Armed { None = 0, Tone = 1, Noise = 2, Synth = 3 };

/// 一个声源：它是什么声音、摆在哪儿、有多响。
///
/// PCM 放在 `shared_ptr` 里，是为了撤销快照便宜 —— 一份快照复制的是指针，不是几 MB 的样本。
struct Source {
    enum class Kind { Tone, Noise, Wav, Synth, Song };
    std::string name;
    Kind kind = Kind::Tone;
    /// 再生这一段声音要的参数（`Wav` / `Song` 不用）。写回 `.scene` 靠的就是它们。
    float frequency = 440.0f;
    float seconds = 0.5f;
    std::uint32_t seed = 1;
    /// `Wav`：这个文件在哪。导出时把它拷到场景旁边要它。
    std::string wav_path;
    /// `Song`：那份 `.song` 在哪。同上，导出时一起搬走。
    std::string song_path;
    std::string source_dir;
    /// `Synth`：整份配方（波形 / 音高 / ADSR / 低通 ……）。改一个数就照它重渲染一遍。
    SynthSpec synth;
    /// `Song`：整首曲子（速度、拍数、乐器、音符）。**音符格子改的就是它** —— 只留一份 clip 的话
    /// 就没法编辑了（编辑要的是那份配方的原件，不是渲染出来的样本）。
    SongSpec song;
    std::shared_ptr<Clip> clip;

    Vec3 position{0.0f, 0.0f, 0.0f};
    float gain = 1.0f;
    float reference = 1.0f;
    float max_distance = 20.0f;
    bool loop = true;
    bool muted = false;
    /// **不跟位置走**（满音量、居中）。放下的声源默认是世界里的；**导入的曲子默认是它** ——
    /// 会跟着位置跑的背景音乐不是背景音乐。按「全局」或者键 `G` 翻过来。
    bool spatial = true;
};

/// 相机怎么绕着看：一个目标点、一个距离、两个角。和建模器同一套。
struct Orbit {
    Vec3 target{0.0f, 0.6f, 0.0f};
    float distance = 7.0f;
    float yaw = 0.0f;
    float pitch = 0.30f;

    Vec3 eye() const {
        const float flat = distance * std::cos(pitch);
        return {target.x + flat * std::sin(yaw), target.y + distance * std::sin(pitch),
                target.z + flat * std::cos(yaw)};
    }
};

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Mul(const Vec3& v, float scale) { return {v.x * scale, v.y * scale, v.z * scale}; }

/// 路径的最后一段（去目录去扩展名），给新声源起名。
std::string BaseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return name.empty() ? std::string("声音") : name;
}

std::string DirOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return path.size() >= 2 && path[1] == ':';
}

/// 一个 MIDI 号的名字（`60` → `C4`），只为了在控制台里说人话。
std::string NoteName(int pitch) {
    static const char* const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                           "F#", "G",  "G#", "A",  "A#", "B"};
    const int clamped = std::clamp(pitch, 0, 127);
    return std::string(kNames[clamped % 12]) + std::to_string(clamped / 12 - 1);
}

/// 试听里的一步：一个音（`pitch < 0` 是**休止**，就是一段静音），和它多长（秒）。
///
/// **一步有多长，就是节奏。** 一首曲子里音符本来就是"什么时候响、响多久"，试听这一串也一样 ——
/// 只是它现在首尾相接、一件乐器，不涉及和声。
struct AuditionStep {
    int pitch = 60;
    float seconds = 0.6f;
};

/// `.scene` 文本里 `sound` 那几行的"源形态"。`ParseScene` 只留下 PCM，留不下"它是怎么来的"——
/// 而重新写出去要的正是那个（一段合成音没法从样本反推回 `sound tone 440 0.5`）。所以读回来的时候
/// 自己再看一遍那几行，顺序和 `Scene::sounds` 一一对应。
struct SoundSpec {
    Source::Kind kind = Source::Kind::Tone;
    float frequency = 440.0f;
    float seconds = 0.5f;
    std::uint32_t seed = 1;
    std::string wav_path;    // 已经按场景目录解过的绝对/可开路径
    std::string song_path;   // 同上，`.song`
    SynthSpec synth;         // kind == Synth 时才有意义
};

std::vector<SoundSpec> ScanSounds(const std::string& text, const std::string& base_dir) {
    std::vector<SoundSpec> specs;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        std::vector<std::string> tokens;
        std::string token;
        while (fields >> token) {
            if (!token.empty() && token[0] == '#') {
                break;
            }
            tokens.push_back(token);
        }
        if (tokens.size() < 2 || tokens[0] != "sound") {
            continue;
        }
        SoundSpec spec;
        if (tokens[1] == "tone" && tokens.size() >= 4) {
            spec.kind = Source::Kind::Tone;
            spec.frequency = std::strtof(tokens[2].c_str(), nullptr);
            spec.seconds = std::strtof(tokens[3].c_str(), nullptr);
        } else if (tokens[1] == "noise" && tokens.size() >= 4) {
            spec.kind = Source::Kind::Noise;
            spec.seconds = std::strtof(tokens[2].c_str(), nullptr);
            spec.seed = static_cast<std::uint32_t>(std::strtoul(tokens[3].c_str(), nullptr, 10));
        } else if (tokens[1] == "wav" && tokens.size() >= 3) {
            spec.kind = Source::Kind::Wav;
            std::string file = tokens[2];
            for (std::size_t index = 3; index < tokens.size(); ++index) {
                file += " " + tokens[index];
            }
            spec.wav_path = IsAbsolutePath(file) ? file : base_dir + "/" + file;
        } else if (tokens[1] == "song" && tokens.size() >= 3) {
            spec.kind = Source::Kind::Song;
            std::string file = tokens[2];
            for (std::size_t index = 3; index < tokens.size(); ++index) {
                file += " " + tokens[index];
            }
            spec.song_path = IsAbsolutePath(file) ? file : base_dir + "/" + file;
        } else if (tokens[1] == "synth") {
            // 和 `ParseScene` 是同一套关键字 —— 那一边认得，这一边也得认。**这里是有意的第二次
            // 解析**：`ParseScene` 只留下 PCM，留不下"它是怎么来的"，而重新写出去要的正是那个。
            spec.kind = Source::Kind::Synth;
            std::size_t at = 2;
            while (at + 1 < tokens.size()) {
                const std::string& key = tokens[at];
                const std::string& text = tokens[at + 1];
                if (key == "wave") {
                    if (text == "square") spec.synth.wave = SynthSpec::Wave::Square;
                    else if (text == "saw") spec.synth.wave = SynthSpec::Wave::Saw;
                    else if (text == "triangle") spec.synth.wave = SynthSpec::Wave::Triangle;
                    else if (text == "noise") spec.synth.wave = SynthSpec::Wave::Noise;
                    else spec.synth.wave = SynthSpec::Wave::Sine;
                } else if (key == "seed") {
                    spec.synth.seed = static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
                } else {
                    const float value = std::strtof(text.c_str(), nullptr);
                    if (key == "f0") spec.synth.start_hz = value;
                    else if (key == "f1") spec.synth.end_hz = value;
                    else if (key == "seconds") spec.synth.seconds = value;
                    else if (key == "attack") spec.synth.attack = value;
                    else if (key == "decay") spec.synth.decay = value;
                    else if (key == "sustain") spec.synth.sustain = value;
                    else if (key == "release") spec.synth.release = value;
                    else if (key == "gain") spec.synth.gain = value;
                    else if (key == "cutoff") spec.synth.cutoff_hz = value;
                }
                at += 2;
            }
        } else {
            continue;   // 认不出的那一种不占一个号（和解析器一致：它本来就会拒掉）
        }
        specs.push_back(spec);
    }
    return specs;
}

/// 放下一个「音效」时的那份默认配方：一声下滑的激光。选它当默认，是因为它一眼就听得出"这是个
/// 音效，不是一个纯音"——有起音、有衰减、音高在往下走。
SynthSpec DefaultZap() {
    SynthSpec spec;
    spec.wave = SynthSpec::Wave::Saw;
    spec.start_hz = 1200.0f;
    spec.end_hz = 300.0f;
    spec.seconds = 0.22f;
    spec.attack = 0.004f;
    spec.decay = 0.06f;
    spec.sustain = 0.25f;
    spec.release = 0.12f;
    spec.gain = 1.0f;
    spec.cutoff_hz = 0.0f;
    return spec;
}

/// 音频器本身。它既是那台机器的宿主，也是这个工具的脑子。
class AudioTool final : public zlong::system::Platform {
public:
    AudioTool(zlong::system::System& machine, const zlong::system::System::Config& config)
        : machine_(machine), config_(config) {
        if (zlong::editor::requested()) {
            editor_ = std::make_unique<zlong::editor::Host>();
        }
        AttachAudio();

        // 一进来就有声音可听：一个低音铺在中间（循环），一个高一点的钟挂在一边。两段都是合成的，
        // 所以这个工具不需要任何素材就能开工。
        AddSynthesised(Source::Kind::Tone, 220.0f, 0.5f, 1, Vec3{0.0f, 0.0f, 0.0f}, "低音");
        AddSynthesised(Source::Kind::Tone, 880.0f, 0.25f, 1, Vec3{3.0f, 0.8f, -4.0f}, "钟");

        Rebuild();
        last_tick_ = std::chrono::steady_clock::now();
    }

    /// 机器的宿主这一半：收输入，改场景。返回 false 就是该收工了。
    bool Pump() override {
        if (editor_ == nullptr) {
            return false;              // 没有编辑器就没有输入，也就没有这次运行
        }
        if (!editor_->Pump()) {
            return false;              // 编辑器把这个程序停了
        }
        bool changed = false;

        for (const zlong::editor::Host::Key& key : editor_->TakeKeys()) {
            if (key.down) {
                changed |= OnKey(key.code);
            }
        }

        // 参数值（`V`）：音量、参考距离、最大距离、master、频率、长度、循环、静音。
        for (const zlong::editor::Host::Value& value : editor_->TakeValues()) {
            changed |= ApplyValue(value);
        }

        // 存取吩咐。路径从管子来，不从命令行来 —— 见 `editor_host.h` 那段。
        for (const zlong::editor::Host::FileOrder& order : editor_->TakeFiles()) {
            using Kind = zlong::editor::Host::FileOrder::Kind;
            switch (order.kind) {
            case Kind::Save:
                SaveScene(order.path);
                break;
            case Kind::Load:
                changed |= LoadScene(order.path);
                break;
            case Kind::Import:
                changed |= ImportWav(order.path);
                break;
            case Kind::Song:
                changed |= ImportSong(order.path);
                break;
            case Kind::SongSave:
                SaveActiveSong(order.path);
                break;
            case Kind::Scene:
                ExportWav(order.path);
                break;
            default:
                break;   // `T` / `J` 是建模器的（贴图、插图片），这里没有意义
            }
        }

        // 面板上「音调」「噪声」点一下：拿到手里，这一次（或下一次）左键就把它放下。
        for (const zlong::editor::Host::Placement& placement : editor_->TakePlacements()) {
            if (placement.kind == 1) {
                armed_ = Armed::Tone;
                std::printf("手里是音调：在画面上左键点一下就放在那里（Esc / 右键取消）\n");
            } else if (placement.kind == 2) {
                armed_ = Armed::Noise;
                std::printf("手里是噪声：在画面上左键点一下就放在那里（Esc / 右键取消）\n");
            } else if (placement.kind == 3) {
                armed_ = Armed::Synth;
                std::printf("手里是音效：在画面上左键点一下就放在那里（Esc / 右键取消）\n");
            }
        }

        // 大纲里点了一行：选中那个声源。
        for (const int index : editor_->TakeSelections()) {
            if (index >= 0 && static_cast<std::size_t>(index) < sources_.size()) {
                active_ = index;
                changed |= true;
                std::printf("选中声源 %d：%s\n", index, sources_[static_cast<std::size_t>(index)].name.c_str());
            }
        }

        for (const zlong::editor::Host::Click& click : editor_->TakeClicks()) {
            if (click.button == kRightButton && click.down) {
                armed_ = Armed::None;
                continue;
            }
            if (click.button != kLeftButton) {
                continue;
            }
            if (click.down) {
                if (armed_ != Armed::None) {
                    changed |= PlaceArmed(armed_, click.x, click.y);
                    armed_ = Armed::None;
                } else {
                    const int picked = Pick(click.x, click.y);
                    if (picked >= 0) {
                        active_ = picked;
                        BeginDrag(click);
                    }
                    changed |= true;
                }
                held_ |= click.button;
            } else {
                EndDrag();
                held_ &= ~click.button;
            }
        }

        const int notches = editor_->TakeWheel();
        if (notches != 0) {
            orbit_.distance *=
                std::exp(-static_cast<float>(notches) * 0.12f);
            orbit_.distance = std::clamp(orbit_.distance, kNearest, kFarthest);
        }

        const auto pointer = editor_->pointer();
        const int dx = pointer.x - last_pointer_.x;
        const int dy = pointer.y - last_pointer_.y;
        if (dx != 0 || dy != 0) {
            if ((held_ & kRightButton) != 0) {
                TurnView(dx, dy);
            } else if ((held_ & kMiddleButton) != 0) {
                Pan(dx, dy);
            } else if ((held_ & kLeftButton) != 0) {
                if (dragging_) {
                    changed |= Drag();
                } else {
                    TurnView(dx, dy);   // 左键拖空处 = 转视角（也就是转头）
                }
            }
            last_pointer_ = pointer;
        }

        Aim();                             // 相机是每帧的数据，改了不用重新上传

        if (changed) {
            Rebuild();
        }
        return true;
    }

    /// 每帧按真实流逝的墙钟混一小块交给声卡。**声卡就是时钟** —— `Submit` 在队列满的时候会挡住，
    /// 于是这一帧的节奏跟着已经播出去的时间走，而不是跟着渲染多快走。
    void OnFrame(Scene&, double) override {
        const auto now = std::chrono::steady_clock::now();
        const double delta = std::chrono::duration<double>(now - last_tick_).count();
        last_tick_ = now;
        if (!playing_ || delta <= 0.0) {
            return;
        }
        const double seconds = std::min(delta, kMaxMixSeconds);
        if (machine_.Mix(seconds)) {
            // 播放头：**自己累加墙钟**。`OnMix` 给的那个秒数每次 `Mix` 都从 0 起（`done / rate`，
            // 见 `内核/system/src/system.cpp`），拿它当播放位置会一直跳回开头。
            played_seconds_ += seconds;
        } else if (mix_notes_++ < 1) {
            std::printf("混音出错了：%s\n", machine_.error().c_str());
        }
        ReportPlay();
    }

    /// 编辑器说"报播放位置"的时候，每一小会儿报一次活动那首曲子播到第几拍了。
    ///
    /// 走的是声源表那条 `\x01zl` 通道（stdout），节流到 50ms 一次 —— 一帧一行太多，而播放头
    /// 差个一两帧没人看得出来。暂停时不报，播放头就停在原地。
    void ReportPlay() {
        if (editor_ == nullptr || !editor_->report_play()) {
            return;
        }
        const Source* source = Active();
        const double song_seconds =
            (source != nullptr && source->kind == Source::Kind::Song) ? source->song.Seconds() : 0.0;
        if (!(song_seconds > 0.0)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report_ < std::chrono::milliseconds(50)) {
            return;
        }
        last_report_ = now;
        const double through = std::fmod(played_seconds_, song_seconds) / song_seconds;
        std::printf("%splay %.4f\n", kSceneMark, through * source->song.loop_beats);
    }

    /// 屏幕上那一帧画好了。`Platform::Present` 的那几个参数，原样交给编辑器。
    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                 std::uint32_t height) override {
        if (editor_ != nullptr) {
            editor_->Present(rgba, pitch, width, height);
        }
    }

private:
    // -- 声音 ---------------------------------------------------------------------------------

    /// 打开声卡，或者退到一个只数帧的 sink。**没有声音也要能开工** —— 画面照常，控制台说清楚。
    void AttachAudio() {
#if defined(_WIN32)
        std::string error;
        device_ = zlong::host::OpenDefaultDevice(error);
        if (device_ != nullptr) {
            sink_ = device_.get();
            const auto* wasapi = dynamic_cast<const zlong::host::WasapiSink*>(device_.get());
            if (wasapi != nullptr) {
                std::printf("声卡：%s（%u Hz）\n", wasapi->device_name().c_str(),
                            wasapi->device_rate());
            }
        } else {
            sink_ = &silent_;
            std::printf("没有声卡（%s）—— 画面照常，只是听不见\n", error.c_str());
        }
#else
        sink_ = &silent_;
        std::printf("这个平台没有声卡 —— 画面照常，只是听不见\n");
#endif
        machine_.set_audio(sink_);
    }

    /// 造一段合成音（音调 / 噪声）并放进世界。
    void AddSynthesised(Source::Kind kind, float frequency, float seconds, std::uint32_t seed,
                        const Vec3& where, const std::string& name) {
        Source source;
        source.kind = kind;
        source.name = name;
        source.frequency = frequency;
        source.seconds = seconds;
        source.seed = seed;
        source.position = where;
        source.clip = std::make_shared<Clip>(
            kind == Source::Kind::Tone ? zlong::engine::MakeTone(frequency, seconds)
                                       : zlong::engine::MakeNoise(seconds, seed));
        sources_.push_back(std::move(source));
        active_ = static_cast<int>(sources_.size()) - 1;
    }

    /// 把活动的（或给定的）声源的合成段重新造一遍 —— 改了频率、长度、波形或者包络之后。
    void Regenerate(Source& source) {
        if (source.kind == Source::Kind::Tone) {
            source.clip =
                std::make_shared<Clip>(zlong::engine::MakeTone(source.frequency, source.seconds));
        } else if (source.kind == Source::Kind::Noise) {
            source.clip =
                std::make_shared<Clip>(zlong::engine::MakeNoise(source.seconds, source.seed));
        } else if (source.kind == Source::Kind::Synth) {
            source.clip = std::make_shared<Clip>(zlong::engine::RenderSynth(source.synth));
        } else if (source.kind == Source::Kind::Song) {
            source.clip = std::make_shared<Clip>(zlong::engine::RenderSong(source.song));
        }
    }

    /// 造一段合成音效并放进世界。**改一个参数就是重渲染一遍** —— spec 是描述，样本是它的结果。
    void AddSynth(const SynthSpec& spec, const Vec3& where, const std::string& name) {
        Source source;
        source.kind = Source::Kind::Synth;
        source.name = name;
        source.synth = spec;
        source.position = where;
        source.clip = std::make_shared<Clip>(zlong::engine::RenderSynth(spec));
        sources_.push_back(std::move(source));
        active_ = static_cast<int>(sources_.size()) - 1;
    }

    // -- 输入 ---------------------------------------------------------------------------------

    bool OnKey(int code) {
        switch (code) {
        case kSpaceKey:
            playing_ = !playing_;
            std::printf(playing_ ? "播放\n" : "暂停\n");
            return false;
        case kEscapeKey:
            armed_ = Armed::None;
            return false;
        case kUpKey:
            return SelectStep(-1);
        case kDownKey:
            return SelectStep(+1);
        case kKey1:
            armed_ = Armed::Tone;
            std::printf("手里是音调：左键放一个\n");
            return false;
        case kKey2:
            armed_ = Armed::Noise;
            std::printf("手里是噪声：左键放一个\n");
            return false;
        case kKey3:
            armed_ = Armed::Synth;
            std::printf("手里是音效：左键放一个\n");
            return false;
        case kKeyX:
            return DeleteActive();
        case kKeyL:
            return ToggleActive(/*loop=*/true);
        case kKeyM:
            return ToggleActive(/*loop=*/false);
        case kKeyG: {
            Source* source = Active();
            if (source == nullptr) {
                return false;
            }
            PushUndo();
            source->spatial = !source->spatial;
            std::printf("%s %s\n", source->name.c_str(),
                        source->spatial ? "跟着位置走" : "全局（满音量、居中）");
            return true;
        }
        case kKeyZ:
            return Undo();
        default:
            return false;
        }
    }

    /// 面板上那些值。**带符号的是"加这个数"**（音量、距离、master），不带符号的是设置或翻转 ——
    /// 面板不知道当前值是多少，所以能加减的就说成加减。
    bool ApplyValue(const zlong::editor::Host::Value& value) {
        Source* source = Active();
        const auto number = [&value](std::size_t index, float fallback) {
            return index < value.numbers.size() ? value.numbers[index] : fallback;
        };
        if (value.name == "master") {
            const float next = std::clamp(master_ + number(0, 0.0f), 0.0f, 4.0f);
            if (std::fabs(next - master_) < 1e-5f) {
                return false;
            }
            PushUndo();
            master_ = next;
            std::printf("整场音量 %.2f\n", static_cast<double>(master_));
            return true;
        }
        if (source == nullptr) {
            std::printf("先选中一个声源\n");
            return false;
        }
        if (value.name == "gain") {
            const float next = std::clamp(source->gain + number(0, 0.0f), 0.0f, 4.0f);
            if (std::fabs(next - source->gain) < 1e-5f) {
                return false;
            }
            PushUndo();
            source->gain = next;
            std::printf("%s 音量 %.2f\n", source->name.c_str(), static_cast<double>(source->gain));
            return true;
        }
        if (value.name == "reference") {
            const float next = std::clamp(source->reference + number(0, 0.0f), 0.0f,
                                          source->max_distance);
            if (std::fabs(next - source->reference) < 1e-5f) {
                return false;
            }
            PushUndo();
            source->reference = next;
            std::printf("%s 参考距离 %.2f\n", source->name.c_str(),
                        static_cast<double>(source->reference));
            return true;
        }
        if (value.name == "max") {
            const float next = std::clamp(source->max_distance + number(0, 0.0f), source->reference,
                                          200.0f);
            if (std::fabs(next - source->max_distance) < 1e-5f) {
                return false;
            }
            PushUndo();
            source->max_distance = next;
            std::printf("%s 最大距离 %.2f\n", source->name.c_str(),
                        static_cast<double>(source->max_distance));
            return true;
        }
        if (value.name == "loop") {
            PushUndo();
            source->loop = !source->loop;
            std::printf("%s %s循环\n", source->name.c_str(), source->loop ? "" : "不");
            return true;
        }
        if (value.name == "mute") {
            PushUndo();
            source->muted = !source->muted;
            std::printf("%s %s\n", source->name.c_str(), source->muted ? "静音" : "取消静音");
            return true;
        }
        if (value.name == "global") {
            // 不跟位置走：满音量、居中。**背景音乐就是这样**，世界里的声音（脚步、钟）不是。
            PushUndo();
            source->spatial = !source->spatial;
            std::printf("%s %s\n", source->name.c_str(),
                        source->spatial ? "跟着位置走" : "全局（满音量、居中）");
            return true;
        }
        if (value.name == "wave") {
            if (source->kind != Source::Kind::Synth) {
                std::printf("%s 不是合成音效，没有波形这一说\n", source->name.c_str());
                return false;
            }
            const int index = std::clamp(static_cast<int>(number(0, 0.0f) + 0.5f), 0, 4);
            const auto wave = static_cast<SynthSpec::Wave>(index);
            if (wave == source->synth.wave) {
                return false;
            }
            PushUndo();
            source->synth.wave = wave;
            Regenerate(*source);
            std::printf("%s 波形 %d（0 正弦 1 方 2 锯 3 三角 4 噪声）\n", source->name.c_str(), index);
            return true;
        }
        if (value.name == "envelope") {
            if (source->kind != Source::Kind::Synth) {
                std::printf("%s 不是合成音效，没有包络这一说\n", source->name.c_str());
                return false;
            }
            const float attack = std::max(0.0f, number(0, source->synth.attack));
            const float decay = std::max(0.0f, number(1, source->synth.decay));
            const float sustain = std::clamp(number(2, source->synth.sustain), 0.0f, 1.0f);
            const float release = std::max(0.0f, number(3, source->synth.release));
            PushUndo();
            source->synth.attack = attack;
            source->synth.decay = decay;
            source->synth.sustain = sustain;
            source->synth.release = release;
            Regenerate(*source);
            std::printf("%s 包络 A %.3f D %.3f S %.2f R %.3f\n", source->name.c_str(),
                        static_cast<double>(attack), static_cast<double>(decay),
                        static_cast<double>(sustain), static_cast<double>(release));
            return true;
        }
        if (value.name == "cutoff") {
            if (source->kind != Source::Kind::Synth) {
                std::printf("%s 不是合成音效，没有低通这一说\n", source->name.c_str());
                return false;
            }
            const float next = std::max(0.0f, number(0, source->synth.cutoff_hz));
            if (std::fabs(next - source->synth.cutoff_hz) < 1e-1f) {
                return false;
            }
            PushUndo();
            source->synth.cutoff_hz = next;
            Regenerate(*source);
            std::printf("%s 低通 %.0f Hz%s\n", source->name.c_str(), static_cast<double>(next),
                        next <= 0.0f ? "（不过滤）" : "");
            return true;
        }
        if (value.name == "freq") {
            if (source->kind == Source::Kind::Synth) {
                // 音效有两个音高：起点和终点。给一个数就是不变（起=终），给两个就是一次滑音。
                const float start = std::clamp(number(0, source->synth.start_hz), 1.0f, 20000.0f);
                const float end = std::clamp(number(1, start), 1.0f, 20000.0f);
                if (std::fabs(start - source->synth.start_hz) < 1e-2f &&
                    std::fabs(end - source->synth.end_hz) < 1e-2f) {
                    return false;
                }
                PushUndo();
                source->synth.start_hz = start;
                source->synth.end_hz = end;
                Regenerate(*source);
                std::printf("%s 音高 %.0f → %.0f Hz\n", source->name.c_str(),
                            static_cast<double>(start), static_cast<double>(end));
                return true;
            }
            if (source->kind != Source::Kind::Tone) {
                std::printf("%s 不是音调也不是音效，改不了频率\n", source->name.c_str());
                return false;
            }
            const float next = std::clamp(number(0, source->frequency), 1.0f, 20000.0f);
            if (std::fabs(next - source->frequency) < 1e-2f) {
                return false;
            }
            PushUndo();
            source->frequency = next;
            Regenerate(*source);
            std::printf("%s 频率 %.0f Hz\n", source->name.c_str(),
                        static_cast<double>(source->frequency));
            return true;
        }
        if (value.name == "length") {
            if (source->kind == Source::Kind::Wav) {
                std::printf("%s 是一段 WAV，长度改不了\n", source->name.c_str());
                return false;
            }
            if (source->kind == Source::Kind::Synth) {
                // 音效的长度是**配方里的**那一个数（不是 `Source::seconds`，那个是给音调/噪声的）。
                const float next = std::clamp(number(0, source->synth.seconds), 0.02f, 30.0f);
                if (std::fabs(next - source->synth.seconds) < 1e-3f) {
                    return false;
                }
                PushUndo();
                source->synth.seconds = next;
                Regenerate(*source);
                std::printf("%s 长度 %.2f 秒\n", source->name.c_str(), static_cast<double>(next));
                return true;
            }
            const float next = std::clamp(number(0, source->seconds), 0.02f, 30.0f);
            if (std::fabs(next - source->seconds) < 1e-3f) {
                return false;
            }
            PushUndo();
            source->seconds = next;
            Regenerate(*source);
            std::printf("%s 长度 %.2f 秒\n", source->name.c_str(),
                        static_cast<double>(source->seconds));
            return true;
        }

        // -- 音符格子那一头 ---------------------------------------------------------------------
        // 一个音符的位置 / 长度 / 音高就是一个值，所以和 `gain`、`envelope` 走同一条 `V`。序号是
        // **发出这一条命令的当下**那个（宿主每改一次就重推一次表，面板跟着重画）。
        //
        // **`songloop`，不是 `loop`** —— 后者是"这个声源循环不循环"那个开关（上面已经有了），
        // 而这里要改的是曲子"一遍多少拍"。一个名字只能有一个意思。
        if (value.name == "note_add" || value.name == "note_move" || value.name == "note_len" ||
            value.name == "note_del" || value.name == "tempo" || value.name == "songloop") {
            if (source->kind != Source::Kind::Song) {
                std::printf("%s 不是曲子，改不了音符\n", source->name.c_str());
                return false;
            }
            SongSpec& song = source->song;
            const auto which = [&value](std::size_t at) {
                return at < value.numbers.size() ? static_cast<int>(value.numbers[at] + 0.5f) : -1;
            };
            const auto inside = [&song](int index) {
                return index >= 0 && static_cast<std::size_t>(index) < song.notes.size();
            };

            if (value.name == "tempo") {
                const double bpm = static_cast<double>(number(0, static_cast<float>(song.tempo_bpm)));
                if (!(bpm > 0.0) || std::fabs(bpm - song.tempo_bpm) < 1e-6) {
                    return false;
                }
                PushUndo();
                song.tempo_bpm = bpm;
                std::printf("速度 %.1f BPM\n", bpm);
            } else if (value.name == "songloop") {
                const double beats =
                    static_cast<double>(number(0, static_cast<float>(song.loop_beats)));
                if (!(beats > 0.0) || std::fabs(beats - song.loop_beats) < 1e-6) {
                    return false;
                }
                PushUndo();
                song.loop_beats = beats;
                std::printf("一遍 %.2f 拍（%.2f 秒）\n", beats, song.Seconds());
            } else if (value.name == "note_del") {
                const int index = which(0);
                if (!inside(index)) {
                    return false;
                }
                PushUndo();
                song.notes.erase(song.notes.begin() + index);
                std::printf("删掉音符 %d（还有 %zu 个）\n", index, song.notes.size());
            } else if (value.name == "note_add") {
                if (song.instruments.empty()) {
                    std::printf("这首曲子还没有乐器，放不了音符\n");
                    return false;
                }
                Note note;
                note.start_beats = std::max(0.0, static_cast<double>(number(0, 0.0f)));
                note.beats = std::max(0.25, static_cast<double>(number(1, 1.0f)));
                note.pitch =
                    std::clamp(static_cast<int>(number(2, 60.0f) + 0.5f), 0, 127);
                note.velocity = std::clamp(number(3, 0.9f), 0.0f, 1.0f);
                const int instrument = which(4);
                note.instrument = (instrument >= 0 &&
                                   static_cast<std::size_t>(instrument) < song.instruments.size())
                                      ? instrument
                                      : 0;
                PushUndo();
                song.notes.push_back(note);
                std::printf("加一个音符 %zu：%.2f 拍起、%.2f 拍长、音高 %d\n", song.notes.size() - 1,
                            note.start_beats, note.beats, note.pitch);
            } else if (value.name == "note_move") {
                const int index = which(0);
                if (!inside(index)) {
                    return false;
                }
                Note& note = song.notes[static_cast<std::size_t>(index)];
                const double start =
                    std::max(0.0, static_cast<double>(number(1, static_cast<float>(note.start_beats))));
                const int pitch =
                    std::clamp(static_cast<int>(number(2, static_cast<float>(note.pitch)) + 0.5f), 0, 127);
                if (std::fabs(start - note.start_beats) < 1e-6 && pitch == note.pitch) {
                    return false;
                }
                PushUndo();
                note.start_beats = start;
                note.pitch = pitch;
            } else {   // note_len
                const int index = which(0);
                if (!inside(index)) {
                    return false;
                }
                Note& note = song.notes[static_cast<std::size_t>(index)];
                const double beats =
                    std::max(0.25, static_cast<double>(number(1, static_cast<float>(note.beats))));
                if (std::fabs(beats - note.beats) < 1e-6) {
                    return false;
                }
                PushUndo();
                note.beats = beats;
            }
            Regenerate(*source);   // **改了音就是为了听** —— 立刻重渲染那一段
            return true;
        }

        // -- 乐器那一头 -------------------------------------------------------------------------
        // 一件乐器是**一口气设整件**的（波形 + ADSR + 增益 + 低通 + 种子）：对话框改一个数就把整件
        // 发一遍，原子，不用逐字段对账。
        if (value.name == "instrument" || value.name == "instrument_add" ||
            value.name == "instrument_del" || value.name == "instrument_play" ||
            value.name == "instrument_stop") {
            if (source->kind != Source::Kind::Song) {
                std::printf("%s 不是曲子，没有乐器可编\n", source->name.c_str());
                return false;
            }
            SongSpec& song = source->song;
            const auto index_at = [&value](std::size_t at) {
                return at < value.numbers.size() ? static_cast<int>(value.numbers[at] + 0.5f) : -1;
            };
            const auto have = [&song](int index) {
                return index >= 0 && static_cast<std::size_t>(index) < song.instruments.size();
            };

            if (value.name == "instrument") {
                const int index = index_at(0);
                if (!have(index) || value.numbers.size() < 9) {
                    return false;
                }
                Instrument edited;
                edited.wave = static_cast<SynthSpec::Wave>(
                    std::clamp(static_cast<int>(value.numbers[1] + 0.5f), 0, 4));
                edited.attack = std::max(0.0f, value.numbers[2]);
                edited.decay = std::max(0.0f, value.numbers[3]);
                edited.sustain = std::clamp(value.numbers[4], 0.0f, 1.0f);
                edited.release = std::max(0.0f, value.numbers[5]);
                edited.gain = std::max(0.0f, value.numbers[6]);
                edited.cutoff_hz = std::max(0.0f, value.numbers[7]);
                edited.seed = static_cast<std::uint32_t>(std::max(0.0f, value.numbers[8]));
                PushUndo();
                song.instruments[static_cast<std::size_t>(index)] = edited;
                std::printf("乐器 %d：%s、A %.3f D %.3f S %.2f R %.3f、增益 %.2f、低通 %.0f\n", index,
                            zlong::engine::SynthWaveName(edited.wave),
                            static_cast<double>(edited.attack), static_cast<double>(edited.decay),
                            static_cast<double>(edited.sustain),
                            static_cast<double>(edited.release), static_cast<double>(edited.gain),
                            static_cast<double>(edited.cutoff_hz));
            } else if (value.name == "instrument_add") {
                PushUndo();
                song.instruments.push_back(Instrument{});
                std::printf("加了一件乐器 %zu\n", song.instruments.size() - 1);
            } else if (value.name == "instrument_del") {
                const int index = index_at(0);
                if (!have(index)) {
                    return false;
                }
                if (song.instruments.size() <= 1) {
                    std::printf("至少要留一件乐器 —— 没有乐器就没地方放音符\n");
                    return false;
                }
                int in_use = 0;
                for (const Note& note : song.notes) {
                    if (note.instrument == index) {
                        ++in_use;
                    }
                }
                if (in_use > 0) {
                    // **不偷偷把那些音符改到别处** —— 一次明确的拒绝比一次意外的重新配器好。
                    std::printf("还有 %d 个音符在用乐器 %d，先把它们改到别的乐器上（或者删掉）\n",
                                in_use, index);
                    return false;
                }
                PushUndo();
                song.instruments.erase(song.instruments.begin() + index);
                // 序号在它后面的音符往前挪一格 —— 不挪的话它们会指到别人身上。
                for (Note& note : song.notes) {
                    if (note.instrument > index) {
                        --note.instrument;
                    }
                }
                std::printf("删掉乐器 %d（还有 %zu 件）\n", index, song.instruments.size());
            } else if (value.name == "instrument_play") {
                const int index = index_at(0);
                if (!have(index)) {
                    return false;
                }
                // 后面是**一对一对的数**：`V instrument_play <乐器> <音高> <秒> <音高> <秒> …`。
                // 音高给 -1 就是**休止**（一段静音）—— 节奏就是"什么时候响、什么时候不响"。
                // 一对都不给就是中央 C、沿用上一次的音长。
                std::vector<AuditionStep> steps;
                for (std::size_t at = 1; at + 1 < value.numbers.size(); at += 2) {
                    AuditionStep step;
                    // **`lround`，不是 `x + 0.5f` 再截断**：休止是 -1，而 (-1 + 0.5) 截断成 0 ——
                    // 一个休止会变成一个"最低的那个音"。向零取整对负数不成立。
                    step.pitch = static_cast<int>(std::lround(value.numbers[at]));
                    step.seconds = value.numbers[at + 1];
                    steps.push_back(step);
                }
                Audition(song, index, steps);
                return true;   // 曲子没变，但要走一趟 Rebuild 才把试听挂上去
            } else {           // instrument_stop
                audition_.reset();
                audition_instrument_ = -1;
                std::printf("停止试听\n");
                return true;
            }

            Regenerate(*source);   // 乐器是这首曲子的一部分声音 —— 整首重渲染
            if (audition_.has_value()) {
                // 正在试听的那件也跟着改（那几一步长短都照旧）。
                Audition(song, audition_instrument_, audition_steps_);
            }
            return true;
        }
        return false;
    }

    // -- 选中、放下、拖动 ---------------------------------------------------------------------

    Source* Active() {
        return (active_ >= 0 && static_cast<std::size_t>(active_) < sources_.size())
                   ? &sources_[static_cast<std::size_t>(active_)]
                   : nullptr;
    }
    const Source* Active() const {
        return (active_ >= 0 && static_cast<std::size_t>(active_) < sources_.size())
                   ? &sources_[static_cast<std::size_t>(active_)]
                   : nullptr;
    }

    bool SelectStep(int step) {
        if (sources_.empty()) {
            return false;
        }
        const int count = static_cast<int>(sources_.size());
        active_ = ((active_ + step) % count + count) % count;
        std::printf("选中声源 %d：%s\n", active_, sources_[static_cast<std::size_t>(active_)].name.c_str());
        return true;
    }

    bool DeleteActive() {
        if (Active() == nullptr) {
            return false;
        }
        PushUndo();
        audition_.reset();             // 删掉的要是正在试听那首，试听也就没了
        audition_instrument_ = -1;
        std::printf("删掉声源 %d：%s\n", active_, sources_[static_cast<std::size_t>(active_)].name.c_str());
        sources_.erase(sources_.begin() + active_);
        active_ = sources_.empty() ? -1 : std::min<int>(active_, static_cast<int>(sources_.size()) - 1);
        return true;
    }

    bool ToggleActive(bool loop) {
        Source* source = Active();
        if (source == nullptr) {
            return false;
        }
        PushUndo();
        if (loop) {
            source->loop = !source->loop;
            std::printf("%s %s循环\n", source->name.c_str(), source->loop ? "" : "不");
        } else {
            source->muted = !source->muted;
            std::printf("%s %s\n", source->name.c_str(), source->muted ? "静音" : "取消静音");
        }
        return true;
    }

    /// 手里那个形状落到画面上：拿这一下的射线打地面，交点就是它摆的地方。
    bool PlaceArmed(Armed armed, int x, int y) {
        Vec3 origin;
        Vec3 direction;
        RayAt(x, y, origin, direction);
        const Vec3 down{0.0f, 1.0f, 0.0f};
        Vec3 where{0.0f, 0.0f, 0.0f};
        if (!PlaneHit(Vec3{0.0f, 0.0f, 0.0f}, origin, direction, down, where)) {
            std::printf("这一下没打到地面，放在中间\n");
        }
        PushUndo();
        if (armed == Armed::Tone) {
            AddSynthesised(Source::Kind::Tone, 440.0f, 0.5f, 1, where, UniqueName("音调"));
        } else if (armed == Armed::Noise) {
            AddSynthesised(Source::Kind::Noise, 440.0f, 0.5f, next_seed_++, where, UniqueName("噪声"));
        } else {
            AddSynth(DefaultZap(), where, UniqueName("音效"));
        }
        std::printf("放下：%s，在 (%.1f, %.1f, %.1f)\n",
                    sources_.back().name.c_str(), static_cast<double>(where.x),
                    static_cast<double>(where.y), static_cast<double>(where.z));
        return true;
    }

    std::string UniqueName(const std::string& base) const {
        for (int suffix = 1;; ++suffix) {
            const std::string candidate = base + " " + std::to_string(suffix);
            bool taken = false;
            for (const Source& source : sources_) {
                if (source.name == candidate) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                return candidate;
            }
        }
    }

    /// 屏幕上隔一个声源最近的、在宽容度里的那一个。**判据是屏幕像素**，不是世界距离。
    int Pick(int x, int y) const {
        int best = -1;
        float best_distance = kPickRadius;
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            float px = 0.0f;
            float py = 0.0f;
            if (!ToPixel(sources_[index].position, px, py)) {
                continue;
            }
            const float dx = px - static_cast<float>(x);
            const float dy = py - static_cast<float>(y);
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance <= best_distance) {
                best_distance = distance;
                best = static_cast<int>(index);
            }
        }
        return best;
    }

    void BeginDrag(const zlong::editor::Host::Click& click) {
        dragging_ = false;
        const Source* source = Active();
        if (source == nullptr) {
            return;
        }
        Vec3 origin;
        Vec3 direction;
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        drag_plane_ = source->position;
        drag_origin_ = source->position;
        RayAt(click.x, click.y, origin, direction);
        if (!PlaneHit(drag_plane_, origin, direction, forward, drag_from_)) {
            return;
        }
        push_undo_pending_ = true;
        dragging_ = true;
    }

    bool Drag() {
        if (!dragging_) {
            return false;
        }
        Source* source = Active();
        if (source == nullptr) {
            return false;
        }
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        Vec3 origin;
        Vec3 direction;
        AimRay(origin, direction);
        Vec3 now;
        if (!PlaneHit(drag_plane_, origin, direction, forward, now)) {
            return false;
        }
        const Vec3 next = Add(drag_origin_, Sub(now, drag_from_));
        if (std::fabs(next.x - source->position.x) < 1e-5f &&
            std::fabs(next.y - source->position.y) < 1e-5f &&
            std::fabs(next.z - source->position.z) < 1e-5f) {
            return false;
        }
        if (push_undo_pending_) {
            PushUndo();
            push_undo_pending_ = false;
        }
        source->position = next;
        return true;
    }

    void EndDrag() {
        dragging_ = false;
        push_undo_pending_ = false;
    }

    // -- 相机 ---------------------------------------------------------------------------------

    void CameraBasis(Vec3& forward, Vec3& right, Vec3& up) const {
        forward = zlong::engine::Normalize(Sub(orbit_.target, orbit_.eye()));
        right = zlong::engine::Normalize(zlong::engine::Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
        up = zlong::engine::Cross(right, forward);
    }

    /// 画面上的一个像素，变成一条射线。和建模器同一套（不走逆矩阵：视线的基就是相机那三个方向）。
    void RayAt(int x, int y, Vec3& origin, Vec3& direction) const {
        const float width = static_cast<float>(config_.width);
        const float height = static_cast<float>(config_.height);
        origin = orbit_.eye();
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        const float half = std::tan(0.5f * scene_.camera.fov_y_radians);
        const float aspect = width / height;
        const float ndc_x = (static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f;
        direction = zlong::engine::Normalize(
            Add(forward, Add(Mul(right, ndc_x * half * aspect), Mul(up, ndc_y * half))));
    }

    void AimRay(Vec3& origin, Vec3& direction) const {
        const auto pointer = editor_ != nullptr ? editor_->pointer() : zlong::editor::Host::Pointer{};
        RayAt(pointer.x, pointer.y, origin, direction);
    }

    /// 一个世界点落在哪个画面像素上 —— `RayAt` 的反面。返回 false 表示它在相机后面。
    bool ToPixel(const Vec3& point, float& x, float& y) const {
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        const Vec3 from_eye = Sub(point, orbit_.eye());
        const float along = zlong::engine::Dot(from_eye, forward);
        if (!(along > 1e-4f)) {
            return false;
        }
        const float half = std::tan(0.5f * scene_.camera.fov_y_radians);
        const float width = static_cast<float>(config_.width);
        const float height = static_cast<float>(config_.height);
        const float ndc_x = (zlong::engine::Dot(from_eye, right) / along) / (half * (width / height));
        const float ndc_y = (zlong::engine::Dot(from_eye, up) / along) / half;
        x = (ndc_x + 1.0f) * 0.5f * width;
        y = (1.0f - ndc_y) * 0.5f * height;
        return true;
    }

    bool PlaneHit(const Vec3& plane, const Vec3& origin, const Vec3& direction, const Vec3& normal,
                  Vec3& hit) const {
        const float denominator = zlong::engine::Dot(direction, normal);
        if (std::fabs(denominator) < 1e-6f) {
            return false;
        }
        const float along = zlong::engine::Dot(Sub(plane, origin), normal) / denominator;
        if (along <= 0.0f) {
            return false;
        }
        hit = Add(origin, Mul(direction, along));
        return true;
    }

    void TurnView(int dx, int dy) {
        orbit_.yaw -= static_cast<float>(dx) * 0.011f;
        orbit_.pitch =
            std::clamp(orbit_.pitch + static_cast<float>(dy) * 0.011f, -kPitchLimit, kPitchLimit);
    }

    void Pan(int dx, int dy) {
        const Vec3 eye = orbit_.eye();
        const Vec3 forward = zlong::engine::Normalize(Sub(orbit_.target, eye));
        const Vec3 right = zlong::engine::Normalize(zlong::engine::Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
        const Vec3 up = zlong::engine::Cross(right, forward);
        const float pixels = orbit_.distance * 0.0016f;
        orbit_.target = Add(orbit_.target,
                            Add(Mul(right, -static_cast<float>(dx) * pixels),
                                Mul(up, static_cast<float>(dy) * pixels)));
    }

    void Aim() {
        for (Scene* scene : {&scene_, &machine_.scene()}) {
            scene->camera.eye = orbit_.eye();
            scene->camera.target = orbit_.target;
            scene->camera.up = {0.0f, 1.0f, 0.0f};
        }
    }

    // -- 拼场景 -------------------------------------------------------------------------------

    /// 把手里那一串声源重新拼成一份场景：地面 + 一个一个球 + 一段一段声音 + 一个一个发射器。
    ///
    /// **整份重新上传是便宜的**（量过：0.07 毫秒），比琢磨"哪一块变了"简单，也保证画出来的和手上
    /// 这份永远一致。**但只在编辑的时候做** —— `Load` 会把 `SoundRenderer` 的游标归零，每帧做就
    /// 等于每帧把声音从头重启（见 `OnFrame` 那段）。
    void Rebuild() {
        scene_ = Scene();
        scene_.background = {0.07f, 0.08f, 0.11f, 1.0f};
        scene_.ambient = {0.30f, 0.31f, 0.36f};
        scene_.lights.push_back(Light{{0.4f, 0.85f, 0.35f}, {0.95f, 0.94f, 0.90f}});
        scene_.camera.eye = orbit_.eye();
        scene_.camera.target = orbit_.target;
        scene_.camera.up = {0.0f, 1.0f, 0.0f};

        // 地面：一块二十见方的网格，给"上下左右"一个参照。
        scene_.meshes.push_back(MakeGrid(20, 20.0f));
        const auto ground_mesh = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
        scene_.materials.push_back(
            Material{{0.15f, 0.16f, 0.19f, 1.0f}, 0.05f, zlong::engine::kNoTexture,
                     zlong::engine::kNoTexture});
        const auto ground_mat = static_cast<std::uint32_t>(scene_.materials.size() - 1);
        scene_.AddNode(ground_mesh, ground_mat,
                       Translation(0.0f, -0.5f, 0.0f) * zlong::engine::Scale(20.0f, 1.0f, 20.0f));

        // 声源那个球，和四份材质：普通、选中、静音、**全局**（不跟位置走）。
        scene_.meshes.push_back(MakeSphere(10, 16));
        const auto marker_mesh = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
        scene_.materials.push_back(
            Material{{0.58f, 0.66f, 0.80f, 1.0f}, 0.35f, zlong::engine::kNoTexture,
                     zlong::engine::kNoTexture});
        const auto marker_mat = static_cast<std::uint32_t>(scene_.materials.size() - 1);
        scene_.materials.push_back(
            Material{{1.0f, 0.62f, 0.18f, 1.0f}, 0.60f, zlong::engine::kNoTexture,
                     zlong::engine::kNoTexture});
        const auto hot_mat = static_cast<std::uint32_t>(scene_.materials.size() - 1);
        scene_.materials.push_back(
            Material{{0.34f, 0.33f, 0.31f, 1.0f}, 0.08f, zlong::engine::kNoTexture,
                     zlong::engine::kNoTexture});
        const auto muted_mat = static_cast<std::uint32_t>(scene_.materials.size() - 1);
        scene_.materials.push_back(
            Material{{0.28f, 0.74f, 0.72f, 1.0f}, 0.35f, zlong::engine::kNoTexture,
                     zlong::engine::kNoTexture});
        const auto global_mat = static_cast<std::uint32_t>(scene_.materials.size() - 1);

        for (std::size_t index = 0; index < sources_.size(); ++index) {
            const Source& source = sources_[index];
            // **增益直接画成大小** —— 响的球大，轻的球小，一眼看得出哪一段在主导。
            const float size = 0.22f + 0.45f * std::clamp(source.gain, 0.0f, 1.5f) / 1.5f;
            const bool selected = static_cast<int>(index) == active_;
            // 选中的永远是暖色（一眼看得出选的是谁）；没选中的按"是什么"上色：全局是青的、
            // 静音是灰的、其余是普通的。
            const auto material = selected ? hot_mat
                                   : source.muted ? muted_mat
                                   : source.spatial ? marker_mat
                                                    : global_mat;
            const float scale = selected ? size * 1.25f : size;
            scene_.AddNode(marker_mesh, material,
                           Translation(source.position.x, source.position.y, source.position.z) *
                               zlong::engine::Scale(scale));
        }

        // 声音和发射器：一段声音一个发射器，序号一一对上（`emitter.sound` 就是声源号）。
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            const Source& source = sources_[index];
            scene_.sounds.push_back(source.clip != nullptr ? *source.clip : Clip{});
            Emitter emitter;
            emitter.sound = source.clip != nullptr ? static_cast<std::int32_t>(index) : kNoSound;
            emitter.local = Translation(source.position.x, source.position.y, source.position.z);
            emitter.gain = source.muted ? 0.0f : source.gain;
            emitter.reference_distance = source.reference;
            emitter.max_distance = source.max_distance;
            emitter.loop = source.loop;
            emitter.spatial = source.spatial;
            scene_.emitters.push_back(emitter);
        }

        // **试听**：永远追加在最后一份 sound + 最后一个 emitter 上。追加在**最后**，现有声源的序号
        // 就一个都不动 —— 靠 `SoundRenderer::Prepare` 那条"同一段接着放"的规矩，整首曲子不会被打断，
        // 试听叠在上面响。它不画球（球只按 `sources_` 走），也不进 `SaveScene`（那也按 `sources_`）。
        if (audition_.has_value()) {
            scene_.sounds.push_back(*audition_);
            Emitter listening;
            listening.sound = static_cast<std::int32_t>(scene_.sounds.size() - 1);
            listening.gain = 1.0f;
            listening.loop = true;
            listening.spatial = false;   // 试听不跟位置走
            scene_.emitters.push_back(listening);
        }
        scene_.master_gain = master_;

        if (!machine_.Load(scene_)) {
            std::printf("场景过不去：%s\n", machine_.error().c_str());
        }
        Aim();
        PublishScene();
    }

    /// 把声源表发回编辑器（`kSceneMark` 打头，编辑器按它分流到大纲视图）。只在真的变了的时候发。
    void PublishScene() {
        const Source* active = Active();
        const bool synth = active != nullptr && active->kind == Source::Kind::Synth;
        const bool song = active != nullptr && active->kind == Source::Kind::Song;

        std::string signature = std::to_string(active_);
        for (const Source& source : sources_) {
            signature += "|";
            signature += source.name;
        }
        // 活动声源要是合成音效，它的配方**也算进签名** —— 改一个数就重发一次，好让面板拿去填对话框。
        if (synth) {
            const SynthSpec& spec = active->synth;
            signature += "|s" + std::to_string(static_cast<int>(spec.wave));
            for (const float value : {spec.start_hz, spec.end_hz, spec.seconds, spec.attack,
                                      spec.decay, spec.sustain, spec.release, spec.cutoff_hz}) {
                signature += " " + std::to_string(value);
            }
        }
        // 是曲子的话，**整串音符也算进去** —— 音符格子每改一个音符都得拿到新的一份（顺序就是序号）。
        if (song) {
            const SongSpec& spec = active->song;
            signature += "|q" + std::to_string(spec.tempo_bpm) + "," +
                         std::to_string(spec.loop_beats);
            for (const Note& note : spec.notes) {
                signature += " " + std::to_string(note.start_beats) + "," +
                             std::to_string(note.beats) + "," + std::to_string(note.pitch) + "," +
                             std::to_string(note.velocity) + "," + std::to_string(note.instrument);
            }
            // 乐器也算进去 —— 改一个参数就得重发（对话框那头的列表和参数要跟着走）。
            for (const Instrument& instrument : spec.instruments) {
                signature += " i" + std::to_string(static_cast<int>(instrument.wave)) + "," +
                             std::to_string(instrument.attack) + "," +
                             std::to_string(instrument.decay) + "," +
                             std::to_string(instrument.sustain) + "," +
                             std::to_string(instrument.release) + "," +
                             std::to_string(instrument.gain) + "," +
                             std::to_string(instrument.cutoff_hz) + "," +
                             std::to_string(instrument.seed);
            }
        }
        if (signature == scene_signature_) {
            return;
        }
        scene_signature_ = signature;
        std::printf("%sbegin\n", kSceneMark);
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            std::printf("%sobj %zu %s\n", kSceneMark, index, sources_[index].name.c_str());
        }
        if (synth) {
            // 波形 + 八个参数，固定顺序 —— 面板那头（`_on_audio_scene`）照这个顺序读。它是**给对话框
            // 填初值**用的：面板本来不知道程序里那份参数是多少（改参数一直是"加减"，没有绝对值）。
            const SynthSpec& spec = active->synth;
            std::printf("%ssynth %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n", kSceneMark,
                        static_cast<int>(spec.wave), static_cast<double>(spec.start_hz),
                        static_cast<double>(spec.end_hz), static_cast<double>(spec.seconds),
                        static_cast<double>(spec.attack), static_cast<double>(spec.decay),
                        static_cast<double>(spec.sustain), static_cast<double>(spec.release),
                        static_cast<double>(spec.cutoff_hz));
        }
        if (song) {
            // 头一行是速度 / 一遍多少拍 / 几件乐器；接着**一件乐器一行**（顺序就是序号），再
            // **一个音符一行**（顺序也是序号）。
            const SongSpec& spec = active->song;
            std::printf("%ssong %.6g %.6g %zu\n", kSceneMark, spec.tempo_bpm, spec.loop_beats,
                        spec.instruments.size());
            for (std::size_t index = 0; index < spec.instruments.size(); ++index) {
                const Instrument& instrument = spec.instruments[index];
                std::printf("%sinstrument %zu %d %.6g %.6g %.6g %.6g %.6g %.6g %u\n", kSceneMark,
                            index, static_cast<int>(instrument.wave),
                            static_cast<double>(instrument.attack),
                            static_cast<double>(instrument.decay),
                            static_cast<double>(instrument.sustain),
                            static_cast<double>(instrument.release),
                            static_cast<double>(instrument.gain),
                            static_cast<double>(instrument.cutoff_hz), instrument.seed);
            }
            for (const Note& note : spec.notes) {
                std::printf("%snote %.6g %.6g %d %.6g %d\n", kSceneMark, note.start_beats,
                            note.beats, note.pitch, static_cast<double>(note.velocity),
                            note.instrument);
            }
        }
        std::printf("%ssel %d\n", kSceneMark, active_);
        std::printf("%send\n", kSceneMark);
    }

    // -- 存取 ---------------------------------------------------------------------------------

    /// 把这一份写成一个 `.scene`：声音是**源形态**（tone / noise / wav / synth 各自的那一行），
    /// 发射器摆在世界坐标里，最后是 master。几何是空 —— 这个工具不管画的东西。
    void SaveScene(const std::string& path) {
        zlong::engine::SceneDescription description;
        description.camera = scene_.camera;
        description.lights = scene_.lights;
        description.ambient = scene_.ambient;
        description.background = scene_.background;
        description.master_gain = master_;

        for (const Source& source : sources_) {
            zlong::engine::SceneSoundInput sound;
            switch (source.kind) {
            case Source::Kind::Tone:
                sound.kind = zlong::engine::SceneSoundInput::Kind::Tone;
                sound.frequency = source.frequency;
                sound.seconds = source.seconds;
                break;
            case Source::Kind::Noise:
                sound.kind = zlong::engine::SceneSoundInput::Kind::Noise;
                sound.seconds = source.seconds;
                sound.seed = source.seed;
                break;
            case Source::Kind::Synth:
                // 存的是**配方**，不是样本 —— 读回来按同一份 spec 重新渲染，样本一定一样。
                sound.kind = zlong::engine::SceneSoundInput::Kind::Synth;
                sound.synth = source.synth;
                break;
            case Source::Kind::Wav:
                // **没有文件就写不出去**：`sound wav` 那一行指着一个文件，指着空气的场景比一次明确
                // 的失败坏得多（和 `SaveModel` 拒绝没有路径的材质一个态度）。
                if (source.wav_path.empty()) {
                    std::printf("「%s」是 WAV，可是不知道它原来在哪个文件，整份不写\n",
                                source.name.c_str());
                    return;
                }
                sound.kind = zlong::engine::SceneSoundInput::Kind::Wav;
                sound.path = source.wav_path;
                sound.source_dir = source.source_dir;
                break;
            case Source::Kind::Song:
                // 同上：`.song` 也得有个文件在，才写得出去。
                if (source.song_path.empty()) {
                    std::printf("「%s」是曲子，可是不知道它原来在哪个文件，整份不写\n",
                                source.name.c_str());
                    return;
                }
                sound.kind = zlong::engine::SceneSoundInput::Kind::Song;
                sound.path = source.song_path;
                sound.source_dir = source.source_dir;
                break;
            }
            description.sounds.push_back(std::move(sound));
        }
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            const Source& source = sources_[index];
            zlong::engine::SceneEmitterInput emitter;
            emitter.sound = static_cast<std::int32_t>(index);
            emitter.position = source.position;
            emitter.gain = source.muted ? 0.0f : source.gain;
            emitter.reference_distance = source.reference;
            emitter.max_distance = source.max_distance;
            emitter.loop = source.loop;
            emitter.spatial = source.spatial;
            description.emitters.push_back(emitter);
        }

        std::string error;
        if (!zlong::engine::SaveScene(path, description, error)) {
            std::printf("存不了：%s\n", error.c_str());
            return;
        }
        std::printf("存好了：%s（%zu 段声音，%zu 个声源）\n", path.c_str(),
                    description.sounds.size(), description.emitters.size());
    }

    /// 读一个 `.scene`，把它**声音那一半**变成一串声源。画的东西（网格、材质、节点）这里不管。
    bool LoadScene(const std::string& path) {
        audition_.reset();             // 换了一份场景，上一首的试听就作废了
        audition_instrument_ = -1;
        Scene loaded;
        std::string error;
        if (!zlong::engine::LoadScene(path, loaded, error)) {
            std::printf("读不了：%s\n", error.c_str());
            return false;
        }
        std::string text;
        if (!zlong::engine::ReadWholeFile(path, text)) {
            std::printf("读不了：%s\n", path.c_str());
            return false;
        }
        const std::vector<SoundSpec> specs = ScanSounds(text, DirOf(path));

        std::vector<Source> rebuilt;
        for (const Emitter& emitter : loaded.emitters) {
            Source source;
            if (emitter.sound >= 0 && static_cast<std::size_t>(emitter.sound) < loaded.sounds.size()) {
                source.clip = std::make_shared<Clip>(loaded.sounds[static_cast<std::size_t>(emitter.sound)]);
            }
            if (emitter.sound >= 0 && static_cast<std::size_t>(emitter.sound) < specs.size()) {
                const SoundSpec& spec = specs[static_cast<std::size_t>(emitter.sound)];
                source.kind = spec.kind;
                source.frequency = spec.frequency;
                source.seconds = spec.seconds;
                source.seed = spec.seed;
                source.wav_path = spec.wav_path;
                source.song_path = spec.song_path;
                source.source_dir = DirOf(spec.wav_path.empty() ? spec.song_path : spec.wav_path);
                source.synth = spec.synth;   // 音效那份配方（不是音效就摆着不用）
                if (source.kind == Source::Kind::Song && !source.song_path.empty()) {
                    // 曲子要**把那份 .song 再读一遍**才拿得到配方 —— 场景里只有渲染好的样本，
                    // 而音符格子编辑的是配方。
                    std::string reason;
                    if (!zlong::engine::LoadSong(source.song_path, source.song, reason)) {
                        std::printf("曲子读不回配方（%s），这一首在格子上编辑不了\n", reason.c_str());
                    }
                }
            }
            std::string base = "声音";
            if (source.kind == Source::Kind::Wav && !source.wav_path.empty()) {
                base = BaseName(source.wav_path);
            } else if (source.kind == Source::Kind::Song && !source.song_path.empty()) {
                base = BaseName(source.song_path);
            } else if (source.kind == Source::Kind::Synth) {
                base = "音效";
            }
            source.name = source.clip == nullptr ? "静音" : UniqueNameIn(rebuilt, base);
            source.position = {emitter.local.m[0][3], emitter.local.m[1][3], emitter.local.m[2][3]};
            source.gain = emitter.gain;
            source.reference = emitter.reference_distance;
            source.max_distance = emitter.max_distance;
            source.loop = emitter.loop;
            source.muted = emitter.gain <= 0.0f;
            source.spatial = emitter.spatial;
            rebuilt.push_back(std::move(source));
        }

        PushUndo();
        sources_ = std::move(rebuilt);
        active_ = sources_.empty() ? -1 : 0;
        master_ = loaded.master_gain;
        std::printf("读了：%s（%zu 个声源）\n", path.c_str(), sources_.size());
        if (sources_.empty()) {
            std::printf("这个场景里没有声音\n");
        }
        return true;
    }

    static std::string UniqueNameIn(const std::vector<Source>& existing, const std::string& base) {
        for (int suffix = 1;; ++suffix) {
            const std::string candidate = base + " " + std::to_string(suffix);
            bool taken = false;
            for (const Source& source : existing) {
                if (source.name == candidate) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                return candidate;
            }
        }
    }

    /// 读一个 WAV 放进世界（`wav_file.h`）。**循环打开** —— 一个背景音效十有八九是要循环的，
    /// 而它就在 `L` 键上，不循环的话一秒就没了。
    bool ImportWav(const std::string& path) {
        auto clip = std::make_shared<Clip>();
        std::string error;
        if (!zlong::engine::LoadWav(path, *clip, error)) {
            std::printf("读不了：%s\n", error.c_str());
            return false;
        }
        PushUndo();
        Source source;
        source.kind = Source::Kind::Wav;
        source.name = UniqueName(BaseName(path));
        source.wav_path = path;
        source.source_dir = DirOf(path);
        source.clip = clip;
        source.position = {0.0f, 0.0f, -2.0f};
        source.loop = true;
        sources_.push_back(std::move(source));
        active_ = static_cast<int>(sources_.size()) - 1;
        std::printf("导入：%s（%.2f 秒，%u Hz）\n", path.c_str(),
                    sources_.back().clip->Seconds(), sources_.back().clip->rate);
        return true;
    }

    /// 读一份 `.song` 放进世界。**默认循环、默认全局** —— 一首背景音乐就该一遍一遍地放，而且
    /// 站在哪儿听都一样。按「全局」或者 `G` 可以把它变回一个世界里的声源。
    bool ImportSong(const std::string& path) {
        audition_.reset();             // 换了一首曲子，上一首的试听就作废了
        audition_instrument_ = -1;
        SongSpec song;
        std::string error;
        if (!zlong::engine::LoadSong(path, song, error)) {
            std::printf("读不了：%s\n", error.c_str());
            return false;
        }
        auto clip = std::make_shared<Clip>(zlong::engine::RenderSong(song));
        if (!clip->Valid()) {
            std::printf("这首曲子里没有音符（或者一遍的长度不是正的），没放进来\n");
            return false;
        }
        PushUndo();
        Source source;
        source.kind = Source::Kind::Song;
        source.name = UniqueName(BaseName(path));
        source.song_path = path;
        source.source_dir = DirOf(path);
        source.song = song;   // 留着那份配方 —— 音符格子改的是它
        source.clip = clip;
        source.loop = true;
        source.spatial = false;   // 背景音乐不跟位置走
        sources_.push_back(std::move(source));
        active_ = static_cast<int>(sources_.size()) - 1;
        std::printf("导入曲子：%s（%zu 个音符、%zu 件乐器、一遍 %.2f 秒、%u Hz）—— 循环、全局\n",
                    path.c_str(), song.notes.size(), song.instruments.size(), song.Seconds(),
                    clip->rate);
        return true;
    }

    /// 开始（或换一件 / 换音高音长 / 换一串）**试听**：拿这件乐器把这几步渲染成一段，**循环响着**。
    ///
    /// 设计音色要的就是"它一直响着，我边调边听"，所以是循环而不是放一次。循环顺手还绕开一个坑：
    /// 同一个声音再按一次试听，靠 `Load` 是重启不了的（指纹没变，游标留着）—— 而循环不需要重启。
    ///
    /// **一步 = 一个音 + 它多长**（`pitch < 0` 是**休止**）。所以试听里有**节奏**：长音短音、该停的
    /// 地方停。实现上很便宜：每一步渲染一份样本（休止就补一段静音），**首尾接起来** —— 于是"一段
    /// 试听"和"一个音"是同一种东西（都是 `Clip`），循环的规矩一个字不用改。
    ///
    /// **换音长、换步数不一样**：那会改样本的帧数，指纹跟着变，于是这一个从头来 —— 本来就该这样。
    /// 只换音高（步数和每一步的长度都不动）帧数没变，游标就留着，声音一点都不打断。
    void Audition(const SongSpec& song, int index, const std::vector<AuditionStep>& steps) {
        if (index < 0 || static_cast<std::size_t>(index) >= song.instruments.size()) {
            return;
        }
        const Instrument& instrument = song.instruments[static_cast<std::size_t>(index)];

        std::vector<AuditionStep> wanted;
        wanted.reserve(steps.size());
        for (const AuditionStep& step : steps) {
            if (wanted.size() >= kAuditionMaxSteps) {
                break;
            }
            AuditionStep made;
            made.pitch = std::clamp(step.pitch, -1, 127);   // -1 = 休止
            made.seconds = std::clamp(step.seconds, kAuditionShortest, kAuditionLongest);
            wanted.push_back(made);
        }
        if (wanted.empty()) {
            wanted.push_back(AuditionStep{60, audition_seconds_});   // 一步都没给就是中央 C
        }

        Clip combined;
        double total = 0.0;
        for (const AuditionStep& step : wanted) {
            total += static_cast<double>(step.seconds);
            if (step.pitch < 0) {
                // 休止：一段静音。**它也要占地方** —— 节奏就是"什么时候响、什么时候不响"。
                const auto quiet = static_cast<std::size_t>(
                    static_cast<double>(step.seconds) * combined.rate);
                combined.frames.insert(combined.frames.end(), quiet, Frame{0.0f, 0.0f});
                continue;
            }
            SynthSpec one;
            one.wave = instrument.wave;
            one.start_hz = static_cast<float>(zlong::engine::PitchHz(step.pitch));
            one.end_hz = one.start_hz;
            one.seconds = step.seconds;
            one.attack = instrument.attack;
            one.decay = instrument.decay;
            one.sustain = instrument.sustain;
            one.release = instrument.release;
            one.gain = instrument.gain;
            one.cutoff_hz = instrument.cutoff_hz;
            one.seed = instrument.seed;
            const Clip part = zlong::engine::RenderSynth(one);
            combined.frames.insert(combined.frames.end(), part.frames.begin(), part.frames.end());
        }
        audition_ = std::move(combined);
        audition_instrument_ = index;
        audition_steps_ = wanted;

        std::string names;
        for (std::size_t i = 0; i < wanted.size() && i < 8; ++i) {
            names += (i == 0 ? "" : " ");
            names += wanted[i].pitch < 0 ? "-" : NoteName(wanted[i].pitch);
        }
        if (wanted.size() > 8) {
            names += " …";
        }
        std::printf("试听乐器 %d（%s，%s，共 %.2f 秒，循环）\n", index,
                    zlong::engine::SynthWaveName(instrument.wave), names.c_str(), total);
    }

    /// 把活动那首曲子写成一份 `.song`。**音符格子改的是内存里那份** —— 不按这个，磁盘上那份还是旧
    /// 的（和建模器里"改了模型要按保存"是同一条规矩）。存过之后这首曲子就住在那儿了。
    void SaveActiveSong(const std::string& path) {
        Source* source = Active();
        if (source == nullptr || source->kind != Source::Kind::Song) {
            std::printf("先选中一首曲子，再存它\n");
            return;
        }
        std::string error;
        if (!zlong::engine::SaveSong(path, source->song, error)) {
            std::printf("存不了：%s\n", error.c_str());
            return;
        }
        source->song_path = path;
        source->source_dir = DirOf(path);
        std::printf("曲子存好了：%s（%zu 个音符、%zu 件乐器、一遍 %.2f 秒）\n", path.c_str(),
                    source->song.notes.size(), source->song.instruments.size(),
                    source->song.Seconds());
    }

    /// 把当前这一份整体混成一个 WAV，用**另一份 `SoundRenderer`** —— 不碰机器里那一路正在响的。
    void ExportWav(const std::string& path) {
        zlong::audio::WavSink sink(path);
        if (!sink.ok()) {
            std::printf("写不了：%s\n", sink.error().c_str());
            return;
        }
        const double seconds = ExportSeconds();
        Scene copy = scene_;
        // 试听永远是最后一份 sound + 最后一个 emitter（见 `Rebuild`）—— 导出的是一首曲子，不该把
        // "我正在试的那件乐器"也录进去。
        if (audition_.has_value() && !copy.sounds.empty() && !copy.emitters.empty()) {
            copy.sounds.pop_back();
            copy.emitters.pop_back();
        }
        zlong::engine::SoundRenderer renderer;
        std::string error;
        if (!renderer.Prepare(copy, error)) {
            std::printf("混不了：%s\n", error.c_str());
            return;
        }
        std::vector<Frame> block(480);
        const auto rate = static_cast<double>(zlong::audio::kSampleRate);
        const auto total = static_cast<std::uint64_t>(seconds * rate);
        std::uint64_t done = 0;
        while (done < total) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(block.size(), total - done));
            if (!renderer.Mix(copy, std::span<Frame>(block.data(), count), error) ||
                !sink.Submit(std::span<const Frame>(block.data(), count))) {
                std::printf("混不了：%s\n", error.empty() ? sink.error().c_str() : error.c_str());
                return;
            }
            done += count;
        }
        sink.Finish();
        std::printf("导出：%s（%.2f 秒，%llu 帧）\n", path.c_str(), seconds,
                    static_cast<unsigned long long>(sink.frames_written()));
    }

    /// 导出多长：够把最长的那一段（不循环的）放完。循环的那些放一个来回就够听出效果了。
    double ExportSeconds() const {
        double longest = 1.0;
        for (const Source& source : sources_) {
            const double length = source.clip != nullptr ? source.clip->Seconds() : 0.0;
            longest = std::max(longest, source.loop ? std::min(length, 4.0) : length);
        }
        return longest;
    }

    // -- 撤销 ---------------------------------------------------------------------------------

    struct Snapshot {
        std::vector<Source> sources;
        int active = -1;
        float master = 1.0f;
    };

    void PushUndo() {
        if (undo_.size() >= kUndoDepth) {
            undo_.erase(undo_.begin());
        }
        undo_.push_back(Snapshot{sources_, active_, master_});
        redo_.clear();
    }

    bool Undo() {
        if (undo_.empty()) {
            std::printf("没有可撤销的了\n");
            return false;
        }
        redo_.push_back(Snapshot{sources_, active_, master_});
        Snapshot step = std::move(undo_.back());
        undo_.pop_back();
        sources_ = std::move(step.sources);
        active_ = step.active;
        master_ = step.master;
        std::printf("撤销\n");
        return true;
    }

    // -- 数据 ---------------------------------------------------------------------------------

    zlong::system::System& machine_;
    zlong::system::System::Config config_;
    std::unique_ptr<zlong::editor::Host> editor_;

    // 设备。开起来了就是它，没开起来就是那个只数帧的 sink —— 两者都活着，`sink_` 指其中一个。
    std::unique_ptr<zlong::audio::Sink> device_;
    zlong::audio::NullSink silent_;
    zlong::audio::Sink* sink_ = nullptr;

    std::vector<Source> sources_;
    int active_ = -1;
    float master_ = 1.0f;
    bool playing_ = true;
    /// 试听：一件乐器的**循环**单音（`nullopt` = 没在试听）。`Rebuild` 把它追加在最后。
    std::optional<Clip> audition_;
    int audition_instrument_ = -1;
    /// 试听那几步（一个音或一个休止 + 它多长）。改内容或长度都会换掉样本的帧数，那一个会从头来。
    std::vector<AuditionStep> audition_steps_;
    /// 一步都没给的时候用的音长（秒）。
    float audition_seconds_ = 0.6f;

    Orbit orbit_;
    Scene scene_;                        // 手上这一份（Rebuild 里拼，Load 交给机器）
    Armed armed_ = Armed::None;
    std::uint32_t next_seed_ = 1;

    // 拖动：从按下那一刻算起，不在上一次结果上累加。
    bool dragging_ = false;
    bool push_undo_pending_ = false;
    Vec3 drag_plane_{};
    Vec3 drag_origin_{};
    Vec3 drag_from_{};

    int held_ = 0;
    zlong::editor::Host::Pointer last_pointer_;

    std::chrono::steady_clock::time_point last_tick_;
    /// 一共播了多少秒（自己累加的墙钟）—— 播放头就是它对曲子长度取模。
    double played_seconds_ = 0.0;
    /// 上一次报播放头是什么时候（节流用）。
    std::chrono::steady_clock::time_point last_report_;
    int mix_notes_ = 0;

    std::string scene_signature_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
};

}  // namespace

int main(int argc, char** argv) {
    // stdout 重定向出去的时候是全缓冲的：跑在编辑器里它那头是一根管子，一个字节不到 4KB 就一直
    // 待在缓冲区里 —— 点一下要等退出才看得见那一行。所以这里不缓冲。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 它没有自己的窗口：画面走面板，输入也走面板。没有编辑器就没有它。
    if (!zlong::editor::requested()) {
        std::printf("音频器要在编辑器里跑 —— 它没有自己的窗口，画面和输入都走那块面板。\n");
        return 1;
    }

    zlong::system::System::Config config;
    config.width = 800;
    config.height = 500;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string name = argv[index];
        const int value = std::atoi(argv[index + 1]);
        if (value > 0 && name == "--width") {
            config.width = static_cast<std::uint32_t>(value);
            ++index;
        } else if (value > 0 && name == "--height") {
            config.height = static_cast<std::uint32_t>(value);
            ++index;
        }
    }
    config.dram_bytes = 512ull * 1024 * 1024;
    config.arena_bytes = 64ull * 1024 * 1024;
    config.vulkan = true;
    // 和建模器一样：它每秒要画几十帧给人看，回落软件光栅器不是"慢一点"，是不能用。
    config.require_vulkan = true;
    config.frame_interval_ms = 16.0;

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::printf("机器起不来：%s\n", machine.error().c_str());
        return 1;
    }
    std::printf("backend: %s\n", machine.backend_name());
    if (!machine.note().empty()) {
        std::printf("note: %s\n", machine.note().c_str());
    }

    AudioTool tool(machine, config);
    machine.set_platform(&tool);

    std::printf("音频器：空格 播放/暂停 · 左键 选/拖 · 右键拖动 转视角（=转头）· "
                "中键拖动 平移 · 滚轮 远近\n");
    const auto started = std::chrono::steady_clock::now();
    const int drawn = machine.Run(-1);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    machine.FinishAudio();

    std::printf("画了 %d 帧，%.1f 秒，%.1f fps\n", drawn, seconds,
                seconds > 0.0 ? static_cast<double>(drawn) / seconds : 0.0);
    return drawn < 0 ? 1 : 0;
}
