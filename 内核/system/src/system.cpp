#include "zlong/system/system.h"

#include <algorithm>
#include <chrono>
#include <span>
#include <thread>
#include <utility>

#include "zlong/audio/format.h"
#include "zlong/gpu/render/backend.h"
#include "zlong/gpu/software/backend.h"

#if defined(ZL_GPU_WITH_VULKAN)
#include "zlong/gpu/vulkan/vk_context.h"
#include "zlong/gpu/vulkan/vk_renderer.h"
#endif

namespace zlong::system {

namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

System::System(const Config& config) : config_(config), ram_(config.dram_bytes) {
    // One physical memory for everything. The GPU decodes out of it, the kernel's
    // allocator hands it out, and the renderer's arena is a slice of it -- the same
    // bytes seen by all three, which is what a unified-memory machine is.
    gpu_ = std::make_unique<gpu::Gpu>(ram_.physical(), *this);

    std::string reason;
    const bool want_vulkan = config_.vulkan || config_.require_vulkan;

#if defined(ZL_GPU_WITH_VULKAN)
    if (want_vulkan) {
        vk_ = gpu::vulkan::VkContext::Create(/*enable_validation=*/false, reason);
        if (vk_ == nullptr) {
            note_ = "vulkan is not usable here (" + reason + ")";
        } else {
            auto vulkan = std::make_unique<gpu::vulkan::VulkanBackend>(*vk_, gpu_->memory());
            if (!vulkan->Initialize(reason)) {
                note_ = "the vulkan backend would not start (" + reason + ")";
                vk_.reset();
            } else {
                backend_ = std::move(vulkan);
            }
        }
    }
#else
    if (want_vulkan) {
        note_ = "this build has no vulkan backend";
    }
#endif

    if (backend_ == nullptr && config_.require_vulkan) {
        // Asked not to fall back, so no backend is the answer. The note is the reason the
        // machine is not coming up, so it becomes the error rather than a side remark.
        error_ = note_.empty() ? std::string("vulkan was required but did not come up") : note_;
        note_.clear();
        return;
    }

    if (backend_ == nullptr) {
        // The software renderer is always available, so a requested backend that would
        // not start is a note and not a failure: the machine still runs. `note_` keeps
        // the reason rather than swallowing it.
        auto software = std::make_unique<gpu::software::SoftwareBackend>(gpu_->memory());
        if (!software->Initialize(reason)) {
            error_ = "the software renderer would not start: " + reason;
            backend_.reset();
            return;
        }
        backend_ = std::move(software);
    }
    gpu_->set_render_backend(backend_.get());

    // The kernel and the device, wired to each other. From here the guest's submissions
    // and the engine's draws reach the same Gpu.
    kernel_ = std::make_unique<service::Kernel>(ram_);
    kernel_->attach_gpu(*gpu_);

    renderer_ = std::make_unique<engine::Renderer>(*gpu_, *backend_);
    error_.clear();
}

System::~System() = default;

void System::set_audio(audio::Sink* sink) noexcept {
    audio_ = sink;
    if (kernel_ != nullptr && sink != nullptr) {
        kernel_->attach_audio(*sink);
    }
}

bool System::Load(const engine::Scene& scene) {
    if (!ok()) {
        return false;
    }
    scene_ = scene;

    // The arena is the kernel's to hand out. The renderer used to bump its own cursor
    // from a base a program chose, which is how two allocators ended up naming the same
    // bytes; now the reservation is one allocation from the same accounting every other
    // consumer uses, and the renderer is refused a scene that would leave it.
    //
    // **One allocation, made once and kept for the machine's life.** It used to be
    // requested on every call, and a block this allocator has handed out never comes back
    // (it is a bump allocator -- see ram::PhysicalMemory), so a second Load quietly took a
    // second `arena_bytes` out of DRAM and left the first one unreachable. A host that
    // loads once per scene never noticed; something that re-loads while the scene is being
    // edited would have run the machine out of memory within a dozen edits.
    if (!arena_.has_value()) {
        const auto reserved =
            ram_.physical().allocate(static_cast<std::size_t>(config_.arena_bytes));
        if (!reserved.has_value()) {
            error_ = "the kernel could not reserve the renderer's arena";
            return false;
        }
        arena_ = static_cast<gpu::GuestPa>(*reserved);
    }
    if (!renderer_->Prepare(scene_, config_.width, config_.height, *arena_, config_.arena_bytes,
                            error_)) {
        return false;
    }
    if (!sound_.Prepare(scene_, error_)) {
        return false;
    }
    prepared_ = true;
    error_.clear();
    return true;
}

System::FrameResult System::Frame() {
    if (!prepared_) {
        error_ = "no scene has been loaded";
        return FrameResult::Failed;
    }
    if (platform_ != nullptr && !platform_->Pump()) {
        return FrameResult::Closed;
    }
    if (platform_ != nullptr) {
        platform_->OnFrame(scene_, last_frame_seconds_);
    }

    const auto started = Clock::now();
    std::size_t draws = 0;
    if (!renderer_->Render(scene_, draws, error_)) {
        return FrameResult::Failed;
    }
    last_frame_seconds_ =
        std::chrono::duration<double>(Clock::now() - started).count();
    draws_ = draws;

    if (platform_ != nullptr) {
        platform_->Present(renderer_->colour_pixels(), renderer_->target_pitch(),
                           renderer_->width(), renderer_->height());
    }
    ++frames_;
    error_.clear();
    return FrameResult::Rendered;
}

int System::Run(int frames) {
    const auto interval = std::chrono::duration<double, std::milli>(config_.frame_interval_ms);
    int drawn = 0;
    while (frames < 0 || drawn < frames) {
        const auto started = Clock::now();
        const FrameResult result = Frame();
        if (result == FrameResult::Failed) {
            return -1;
        }
        if (result == FrameResult::Closed) {
            break;
        }
        ++drawn;

        // The machine paces itself. A host that wants a different rate says so in the
        // Config rather than sleeping in its own loop, because the loop is not its.
        if (config_.frame_interval_ms > 0.0) {
            const auto spent = Clock::now() - started;
            if (spent < interval) {
                std::this_thread::sleep_for(interval - spent);
            }
        }
    }
    return drawn;
}

bool System::Mix(double seconds) {
    if (!prepared_) {
        error_ = "no scene has been loaded";
        return false;
    }
    if (audio_ == nullptr) {
        error_ = "no audio device is attached";
        return false;
    }
    if (config_.audio_block_frames == 0) {
        error_ = "the audio block size is zero";
        return false;
    }

    std::vector<audio::Frame> block(config_.audio_block_frames);
    const auto rate = static_cast<double>(audio::kSampleRate);
    const auto total = static_cast<std::uint64_t>(seconds * rate);
    std::uint64_t done = 0;
    while (done < total) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), total - done));
        if (platform_ != nullptr) {
            platform_->OnMix(scene_, static_cast<double>(done) / rate);
        }
        if (!sound_.Mix(scene_, std::span<audio::Frame>(block.data(), count), error_)) {
            return false;
        }
        if (!audio_->Submit(std::span<const audio::Frame>(block.data(), count))) {
            error_ = "the audio device would not take the frames";
            return false;
        }
        done += count;
    }
    error_.clear();
    return true;
}

bool System::FinishAudio() {
    if (audio_ == nullptr) {
        return true;
    }
    if (!audio_->Finish()) {
        error_ = "the audio device could not be finished";
        return false;
    }
    error_.clear();
    return true;
}

void System::OnGpuFault(const gpu::GpuFault& fault) {
    // Kept rather than printed: a host can report them, and a fault is the whole reason
    // a frame comes out empty, so it has to be answerable for after the fact.
    fault_log_.push_back(fault);
}

void System::OnSyncpointSignal(gpu::SyncpointId id, std::uint32_t value) {
    (void)id;
    (void)value;
    ++syncpoint_signals_;
}

}  // namespace zlong::system
