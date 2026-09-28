// 烛龙 (ZhuLong) - the GPU layer.
//
// Renders into the guest's framebuffer in guest memory: there is deliberately
// no window, no swapchain and no presentation here. Display is a separate,
// later concern.
//
// The layer never implements cpu::CpuHost. The kernel owns the mapping from
// guest syscalls to Gpu::SubmitPushbuffer, and owns the decision to park a core
// or raise an interrupt.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "zlong/gpu/engine/maxwell3d.h"
#include "zlong/gpu/gpu_host.h"
#include "zlong/gpu/memory/gpu_address_space.h"
#include "zlong/gpu/memory/gpu_memory.h"
#include "zlong/gpu/render/backend.h"
#include "zlong/gpu/types.h"
#include "zlong/ram/physical.h"

namespace zlong::gpu {

namespace vulkan {
class VkContext;
}  // namespace vulkan

// ChannelId comes from types.h. A channel's register file is why submissions name
// one: the colour target, viewport, depth state, selected shader and pending draw
// all belong to the channel, not to the device.

/// What one submitted command stream amounted to.
enum class SubmitStatus : std::uint8_t {
    Ok,
    /// The command buffer is not, or not wholly, guest RAM.
    BufferNotMapped,
    /// The stream was structurally invalid: truncated, or an unknown opcode.
    MalformedStream,
    /// A method the engine needed was malformed. The stream stopped there.
    MalformedMethod,
    /// A draw was assembled but no render backend is installed.
    NoBackend,
    /// The submission named a channel that is not open.
    NoChannel,
    /// The backend refused the draw, or the device was lost.
    RenderFailed,
};

const char* ToString(SubmitStatus status) noexcept;

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Ok;
    /// Methods decoded, including the ones the engine skipped.
    std::size_t methods = 0;
    /// Draws executed.
    std::size_t draws = 0;
    /// Why, when the status is not Ok.
    std::string detail;

    bool ok() const noexcept { return status == SubmitStatus::Ok; }
};

class Gpu {
public:
    Gpu(ram::PhysicalMemory& physical, GpuHost& host);
    ~Gpu();

    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    /// Bring up Vulkan and run the capability probe. Returns false (with
    /// last_error() set) when no usable device exists; the caller may continue
    /// without a GPU.
    bool Initialize(bool enable_validation = false);
    void Shutdown();

    bool ready() const noexcept {
#if defined(ZL_GPU_WITH_VULKAN)
        return vk_ != nullptr;
#else
        return false;
#endif
    }
    const std::string& last_error() const noexcept { return error_; }

#if defined(ZL_GPU_WITH_VULKAN)
    vulkan::VkContext* vk() noexcept { return vk_.get(); }
#endif
    ram::PhysicalMemory& physical() noexcept { return physical_; }
    GpuHost& host() noexcept { return host_; }

    /// The guest memory view the engine decodes out of and every backend reads
    /// through. One place, so a GPU write still reaches the code-write observer.
    GpuMemoryManager& memory() noexcept { return memory_; }
    const GpuMemoryManager& memory() const noexcept { return memory_; }

    /// The address space every GPU address a stream names is resolved through. One
    /// per device, which is what the hardware has: nvmap hands out addresses in it and
    /// the command decoder looks them up in it.
    GpuAddressSpace& address_space() noexcept { return address_space_; }
    const GpuAddressSpace& address_space() const noexcept { return address_space_; }

    /// The channel the host-side engine and the tests submit through. A guest
    /// opens its own with CreateChannel; this one exists so a caller driving a
    /// single stream does not have to name a channel at all.
    static constexpr ChannelId kEngineChannel = 0;

    /// A fresh register file. The handle stays valid until DestroyChannel, and
    /// kInvalidChannel is returned when there is nothing to hand out.
    ChannelId CreateChannel();
    void DestroyChannel(ChannelId handle);

    /// Install the backend every draw is executed on. Borrowed, and may be null:
    /// a stream then still decodes, and each draw is reported as NoBackend rather
    /// than silently dropped.
    void set_render_backend(render::RenderBackend* backend) noexcept { backend_ = backend; }
    render::RenderBackend* render_backend() const noexcept { return backend_; }

    /// Bind the shaders a stream selects by slot. Borrowed: the caller owns the
    /// modules and has to keep them alive while a stream may draw with them.
    void SetShader(ChannelId channel, std::size_t slot, const shader::Module* vertex,
                   const shader::Module* fragment);

    /// Decode and run one command stream out of guest physical memory.
    ///
    /// Each draw the stream assembles is consumed and executed before the walk
    /// goes on. That is not a shortcut: the engine holds one pending draw, so a
    /// stream with two of them would otherwise keep only the last.
    ///
    /// Faults are reported through GpuHost and also returned. The layer reports;
    /// the kernel decides what the guest is told.
    SubmitResult SubmitPushbuffer(ChannelId channel, GuestPa address, std::uint64_t size);

    // Convenience for a single-stream caller: these mean the engine channel.
    void SetShader(std::size_t slot, const shader::Module* vertex, const shader::Module* fragment) {
        SetShader(kEngineChannel, slot, vertex, fragment);
    }
    SubmitResult SubmitPushbuffer(GuestPa address, std::uint64_t size) {
        return SubmitPushbuffer(kEngineChannel, address, size);
    }

    /// What to run when a submission finishes. Called on the render worker.
    using Completion = std::function<void(const SubmitResult&)>;

    /// Hand a stream to the render worker and return at once.
    ///
    /// This is the shape a guest driver needs. Rendering on the submitting core --
    /// which is what SubmitPushbuffer does, and all the host engine ever needs --
    /// spends a whole guest core drawing and leaves the guest no way to do anything
    /// else; a driver submits and then waits on a syncpoint instead. `on_done` runs
    /// on the worker after this submission's draws have been published, in
    /// submission order, so a syncpoint advanced from it means what it says.
    void SubmitPushbufferAsync(ChannelId channel, GuestPa address, std::uint64_t size,
                               Completion on_done);

    /// One GPFIFO entry: where a command buffer is, and how long it is.
    ///
    /// Hardware's entry is two words and the segment it names runs until the GPU meets
    /// an end-of-segment marker. Ours carries the length as a third word instead --
    /// stated rather than guessed at, and it keeps the command stream itself free of a
    /// marker the console may not have.
    static constexpr std::size_t kGpfifoEntryWords = 3;
    static constexpr std::size_t kGpfifoEntryBytes =
        kGpfifoEntryWords * sizeof(std::uint32_t);

    /// Decode and run a GPFIFO: a range inside the channel's bound GPFIFO buffer,
    /// interpreted as entries, each naming a command buffer.
    ///
    /// This is the layer a driver submits through. The entry addresses are GPU
    /// addresses and every one is resolved -- a GPFIFO that names a command buffer the
    /// device was never told about is a fault, exactly as it is inside a stream.
    /// SubmitPushbuffer stays the way to execute one command stream directly, which is
    /// what the host engine does.
    SubmitResult SubmitGpfifo(ChannelId channel, GuestPa gpfifo, std::uint64_t byte_offset,
                              std::uint64_t byte_size);

    /// The asynchronous form, for the guest's own submission path.
    void SubmitGpfifoAsync(ChannelId channel, GuestPa gpfifo, std::uint64_t byte_offset,
                           std::uint64_t byte_size, Completion on_done);

    /// Run everything queued and wait for the worker to go idle.
    void WaitForIdle();

    /// Where the last submission's time went, in milliseconds. Decoding a stream and
    /// running it are different costs with different fixes, so they are counted apart.
    struct Profile {
        /// Walking the stream into draw descriptions.
        double decode_ms = 0.0;
        /// Running those draws on the backend.
        double execute_ms = 0.0;
        /// Making the result visible.
        double flush_ms = 0.0;
    };
    const Profile& profile() const noexcept { return profile_; }

private:
    /// One channel's state: its own register file, its own pending draw.
    struct Channel {
        engine::Maxwell3D engine;
    };

    struct Job {
        ChannelId channel = kEngineChannel;
        GuestPa address = 0;
        std::uint64_t size = 0;
        /// When set, `address`/`size` name a GPFIFO entry list rather than one command
        /// stream.
        bool gpfifo = false;
        Completion on_done;
    };

    /// Run one queued job on the worker.
    SubmitResult Run(const Job& job);

    Channel* FindChannel(ChannelId handle);
    bool ExecuteDraw(const render::DrawDesc& draw, SubmitResult& result);
    void ReportFault(GpuFaultKind kind, GuestPa address, std::uint32_t method,
                     const std::string& detail);

    /// Start the render worker if it is not running. Only an asynchronous caller
    /// pays for the thread.
    void EnsureWorker();
    void RenderWorker();

    ram::PhysicalMemory& physical_;
    GpuHost& host_;
    GpuMemoryManager memory_;
    GpuAddressSpace address_space_;
    std::map<ChannelId, std::unique_ptr<Channel>> channels_;
    ChannelId next_channel_ = kEngineChannel + 1;
    render::RenderBackend* backend_ = nullptr;
#if defined(ZL_GPU_WITH_VULKAN)
    std::unique_ptr<vulkan::VkContext> vk_;
#endif
    std::string error_;
    Profile profile_{};

    // The queue. `queue_mutex_` guards all four; the backend itself is only ever
    // touched by the worker or by a synchronous caller, never by both at once
    // because a synchronous caller is the one holding the submission.
    std::mutex queue_mutex_;
    std::condition_variable queue_ready_;
    std::condition_variable queue_idle_;
    std::deque<Job> queue_;
    /// Jobs the worker is inside right now.
    std::size_t running_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace zlong::gpu
