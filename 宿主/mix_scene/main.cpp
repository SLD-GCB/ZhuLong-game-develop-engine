// 烛龙 (ZhuLong) - mix a scene to a file or to the sound card.
//
// The audio counterpart of `render_scene`, and deliberately as thin. The machine is
// 内核/system: it owns the mixer, the device and the loop; this program describes a
// scene and says how long to mix. Everything about how a scene becomes samples lives in
// 引擎/, and the loop that drives it is the machine's, not this program's.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "zlong/audio/sink.h"
#include "zlong/audio/wav_sink.h"
#include "zlong/engine/scene_file.h"
#include "zlong/system/system.h"

#if defined(_WIN32)
#include "wasapi_sink.h"
#endif

namespace {

using zlong::engine::Emitter;
using zlong::engine::Identity;
using zlong::engine::kNoMesh;
using zlong::engine::kNoParent;
using zlong::engine::MakeNoise;
using zlong::engine::MakeTone;
using zlong::engine::Scene;
using zlong::engine::Translation;
using zlong::engine::Vec3;

constexpr float kTwoPi = 6.28318530717958647692f;
/// Where in the node list the chime's transform group lands, so the sweep below can
/// move it. BuiltInScene adds exactly one node.
constexpr std::size_t kChimeGroup = 0;

/// A low looping bed at the listener and a chime that sweeps across the front, so one
/// run exercises the loop, the falloff, the pan and the transform tree.
Scene BuiltInScene() {
    Scene scene;
    scene.camera.eye = Vec3{0.0f, 0.0f, 0.0f};
    scene.camera.target = Vec3{0.0f, 0.0f, -1.0f};
    scene.camera.up = Vec3{0.0f, 1.0f, 0.0f};

    // 220 Hz for half a second and 880 Hz for a quarter, both at half scale: a bed and a
    // chime that do not need a file to be audible.
    scene.sounds.push_back(MakeTone(220.0f, 0.5f));
    scene.sounds.push_back(MakeTone(880.0f, 0.25f));
    // And a little noise, so the mix is not three sine waves.
    scene.sounds.push_back(MakeNoise(0.25f, /*seed=*/7u));

    // The bed loops at the listener, quieter than the chime.
    const std::int32_t bed = scene.AddEmitter(0, Identity(), kNoParent, 0.35f);
    scene.emitters[bed].loop = true;

    // The chime hangs off a transform group, which the platform below slides from side
    // to side. That the chime follows it is the transform tree doing its job.
    scene.AddNode(kNoMesh, 0, Translation(0.0f, 0.0f, -5.0f));
    const std::int32_t chime = scene.AddEmitter(1, Identity(), static_cast<std::int32_t>(kChimeGroup));
    scene.emitters[chime].max_distance = 12.0f;
    scene.emitters[chime].loop = true;

    // The noise sits behind the listener and off to one side, where the bed does not
    // mask it and the falloff has something to do.
    const std::int32_t hiss = scene.AddEmitter(2, Translation(-3.0f, 1.0f, 2.0f));
    scene.emitters[hiss].reference_distance = 1.0f;
    scene.emitters[hiss].max_distance = 8.0f;
    scene.emitters[hiss].loop = true;
    return scene;
}

/// The platform half, and the only thing this program does beyond describing a scene:
/// slide the chime's group across the listener, once across the whole run. The phase is
/// the same one the loop used to compute by hand -- seconds mixed over seconds total --
/// so the mix is unchanged by the loop moving to the machine.
class SweepPlatform final : public zlong::system::Platform {
public:
    SweepPlatform(bool animate, double total_seconds)
        : animate_(animate), total_seconds_(total_seconds) {}

    void OnMix(Scene& scene, double seconds_mixed) override {
        if (!animate_ || scene.nodes.size() <= kChimeGroup) {
            return;
        }
        const double total = total_seconds_ <= 0.0 ? 1.0 : total_seconds_;
        const auto phase = static_cast<float>(seconds_mixed / total);
        scene.nodes[kChimeGroup].local =
            Translation(std::sin(phase * kTwoPi) * 4.0f, 0.0f, -5.0f);
    }

private:
    bool animate_ = false;
    double total_seconds_ = 1.0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string output = "scene.wav";
    std::string scene_path;
    double seconds = 2.0;
    bool play = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--seconds" && index + 1 < argc) {
            seconds = std::atof(argv[++index]);
        } else if (argument == "--scene" && index + 1 < argc) {
            scene_path = argv[++index];
        } else if (argument == "--device") {
            play = true;
        } else {
            output = argument;
        }
    }

    std::string error;
    Scene scene;
    // Only the built-in scene has the transform group the sweep below moves. A scene
    // read from a file is mixed exactly as it describes itself -- the same rule the
    // renderer follows for the scene files it reads.
    bool animate = false;
    if (scene_path.empty()) {
        scene = BuiltInScene();
        animate = true;
    } else if (!zlong::engine::LoadScene(scene_path, scene, error)) {
        std::fprintf(stderr, "the scene %s could not be loaded: %s\n", scene_path.c_str(),
                     error.c_str());
        return 1;
    }
    std::printf("scene: %zu sound(s), %zu emitter(s), %.2f s at %u Hz\n", scene.sounds.size(),
                scene.emitters.size(), seconds, zlong::audio::kSampleRate);

    zlong::system::System machine({});
    if (!machine.ok()) {
        std::fprintf(stderr, "the machine could not be built: %s\n", machine.error().c_str());
        return 1;
    }

    // A device is the clock, so mixing into one is what makes this play in real time; a
    // file mixes as fast as it can be written. Either way the machine does not know which
    // it got -- it hands the same frames to a Sink.
    std::unique_ptr<zlong::audio::Sink> sink;
    if (play) {
#if defined(_WIN32)
        sink = zlong::host::OpenDefaultDevice(error);
        if (sink == nullptr) {
            std::fprintf(stderr, "the sound card could not be opened: %s\n", error.c_str());
            return 1;
        }
#else
        std::fprintf(stderr, "this build has no device sink; give it a file instead\n");
        return 1;
#endif
    } else {
        auto file = std::make_unique<zlong::audio::WavSink>(output);
        if (!file->ok()) {
            std::fprintf(stderr, "the file could not be opened: %s\n", file->error().c_str());
            return 1;
        }
        sink = std::move(file);
    }
    // Handed to the machine, which wires it into the kernel's services and the engine
    // alike, so a game and the engine are heard through one sink.
    machine.set_audio(sink.get());

    if (!machine.Load(scene)) {
        std::fprintf(stderr, "the scene could not be prepared: %s\n", machine.error().c_str());
        return 1;
    }

    SweepPlatform platform(animate, seconds);
    machine.set_platform(&platform);

    const auto started = std::chrono::steady_clock::now();
    if (!machine.Mix(seconds)) {
        std::fprintf(stderr, "the scene could not be mixed: %s\n", machine.error().c_str());
        return 1;
    }
    if (!machine.FinishAudio()) {
        std::fprintf(stderr, "%s\n", machine.error().c_str());
        return 1;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);

    // The wall clock is the check that matters for a device: mixing into a file takes
    // about as long as writing it, and mixing into a sound card takes as long as the
    // audio lasts, because the card -- not this program -- decides how fast it goes.
    const auto mixed =
        static_cast<std::uint64_t>(seconds * static_cast<double>(zlong::audio::kSampleRate));
    std::printf("mixed %llu frame(s) through %zu emitter(s) into %s in %.2f s\n",
                static_cast<unsigned long long>(mixed), machine.sound().playing(),
                play ? sink->name() : output.c_str(), elapsed.count());
#if defined(_WIN32)
    if (play) {
        const auto* device = dynamic_cast<const zlong::host::WasapiSink*>(sink.get());
        if (device != nullptr) {
            std::printf("  device: %s at %u Hz, %llu frame(s) played, %llu silent, %llu underrun(s)\n",
                        device->device_name().c_str(), device->device_rate(),
                        static_cast<unsigned long long>(device->frames_played()),
                        static_cast<unsigned long long>(device->silence_frames()),
                        static_cast<unsigned long long>(device->underruns()));
        }
    }
#endif
    return 0;
}
