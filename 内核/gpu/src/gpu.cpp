#include "zlong/gpu/gpu.h"

#include <chrono>
#include <optional>

#include "zlong/gpu/engine/pushbuffer.h"

#if defined(ZL_GPU_WITH_VULKAN)
#include "zlong/gpu/vulkan/vk_context.h"
#endif

namespace zlong::gpu {

const char* ToString(SubmitStatus status) noexcept {
    switch (status) {
    case SubmitStatus::Ok:
        return "ok";
    case SubmitStatus::BufferNotMapped:
        return "buffer not mapped";
    case SubmitStatus::MalformedStream:
        return "malformed stream";
    case SubmitStatus::MalformedMethod:
        return "malformed method";
    case SubmitStatus::NoBackend:
        return "no render backend";
    case SubmitStatus::NoChannel:
        return "no such channel";
    case SubmitStatus::RenderFailed:
        return "render failed";
    }
    return "unknown";
}

Gpu::Gpu(ram::PhysicalMemory& physical, GpuHost& host)
    : physical_(physical), host_(host), memory_(physical) {
    // The engine channel always exists, so a caller that never names one has
    // somewhere to submit.
    auto engine = std::make_unique<Channel>();
    engine->engine.set_address_space(&address_space_);
    channels_[kEngineChannel] = std::move(engine);
}

Gpu::~Gpu() {
    Shutdown();
}

ChannelId Gpu::CreateChannel() {
    const ChannelId handle = next_channel_++;
    auto created = std::make_unique<Channel>();
    // One address space per device, shared by every channel: nvmap maps into it and
    // each channel resolves through it.
    created->engine.set_address_space(&address_space_);
    // A new channel starts with the shader library the host installed on the engine
    // channel -- see Maxwell3D::CopyShadersFrom for why that is a copy and not a
    // share.
    if (const auto found = channels_.find(kEngineChannel); found != channels_.end()) {
        created->engine.CopyShadersFrom(found->second->engine);
    }
    channels_[handle] = std::move(created);
    return handle;
}

void Gpu::DestroyChannel(ChannelId handle) {
    if (handle == kEngineChannel) {
        return;  // the engine's own channel outlives any guest
    }
    channels_.erase(handle);
}

Gpu::Channel* Gpu::FindChannel(ChannelId handle) {
    const auto found = channels_.find(handle);
    return found == channels_.end() ? nullptr : found->second.get();
}

void Gpu::SetShader(ChannelId channel, std::size_t slot, const shader::Module* vertex,
                    const shader::Module* fragment) {
    Channel* target = FindChannel(channel);
    if (target != nullptr) {
        target->engine.SetShader(slot, vertex, fragment);
    }
}

bool Gpu::Initialize(bool enable_validation) {
    Shutdown();

#if defined(ZL_GPU_WITH_VULKAN)
    std::string error;
    vk_ = vulkan::VkContext::Create(enable_validation, error);
    if (vk_ == nullptr) {
        error_ = error.empty() ? "Vulkan device creation failed" : error;
        return false;
    }
    error_.clear();
    return true;
#else
    // Built without the Vulkan backend: the CPU render path is the only option.
    (void)enable_validation;
    error_ = "built without the Vulkan backend (ZL_GPU_WITH_VULKAN=OFF)";
    return false;
#endif
}

void Gpu::Shutdown() {
    // The worker is a user of the backend, so it has to be gone before the device is.
    // It drains what is queued first: a submission that was accepted is a submission
    // someone may be waiting on a syncpoint for.
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stopping_ = true;
        }
        queue_ready_.notify_all();
        worker_.join();
    }

#if defined(ZL_GPU_WITH_VULKAN)
    vk_.reset();
#endif
}

void Gpu::EnsureWorker() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!worker_.joinable()) {
        stopping_ = false;
        worker_ = std::thread([this] { RenderWorker(); });
    }
}

void Gpu::SubmitPushbufferAsync(ChannelId channel, GuestPa address, std::uint64_t size,
                               Completion on_done) {
    EnsureWorker();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        Job job;
        job.channel = channel;
        job.address = address;
        job.size = size;
        job.on_done = std::move(on_done);
        queue_.push_back(std::move(job));
    }
    queue_ready_.notify_one();
}

SubmitResult Gpu::SubmitGpfifo(ChannelId channel, GuestPa gpfifo, std::uint64_t byte_offset,
                               std::uint64_t byte_size) {
    SubmitResult total;

    if (FindChannel(channel) == nullptr) {
        total.status = SubmitStatus::NoChannel;
        total.detail = "no such channel";
        return total;
    }
    const std::uint64_t entries = byte_size / kGpfifoEntryBytes;
    if (entries == 0) {
        // A GPFIFO with no whole entry in it is nothing to do, not a fault: a driver
        // that rounded its count down to zero submitted no work.
        return total;
    }

    const std::uint8_t* bytes = memory_.peek(gpfifo + byte_offset, entries * kGpfifoEntryBytes);
    if (bytes == nullptr) {
        total.status = SubmitStatus::BufferNotMapped;
        total.detail = "the GPFIFO is not mapped guest memory";
        ReportFault(GpuFaultKind::AddressTranslation, gpfifo + byte_offset, 0, total.detail);
        return total;
    }

    for (std::uint64_t entry = 0; entry < entries; ++entry) {
        const std::uint32_t* words =
            reinterpret_cast<const std::uint32_t*>(bytes) + entry * kGpfifoEntryWords;
        const GpuVAddr name = static_cast<GpuVAddr>(words[0]) |
                              (static_cast<GpuVAddr>(words[1]) << 32);
        const std::uint64_t size = words[2];

        // An entry names a command buffer in the GPU's address space, so it is resolved
        // like every other address the device is handed.
        const std::optional<GuestPa> physical = address_space_.Resolve(name, size);
        if (!physical.has_value()) {
            total.status = SubmitStatus::BufferNotMapped;
            total.detail = "the GPFIFO names GPU address " + std::to_string(name) +
                           ", which was never mapped";
            ReportFault(GpuFaultKind::AddressTranslation, gpfifo + byte_offset, 0, total.detail);
            return total;
        }

        const SubmitResult part = SubmitPushbuffer(channel, *physical, size);
        total.methods += part.methods;
        total.draws += part.draws;
        if (!part.ok()) {
            // A segment the engine refused stops the GPFIFO here, the way a malformed
            // stream stops a walk: the entries after it were never reached.
            total.status = part.status;
            total.detail = part.detail;
            return total;
        }
    }

    return total;
}

void Gpu::SubmitGpfifoAsync(ChannelId channel, GuestPa gpfifo, std::uint64_t byte_offset,
                            std::uint64_t byte_size, Completion on_done) {
    EnsureWorker();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        Job job;
        job.channel = channel;
        job.address = gpfifo;
        // The worker re-derives the offset from `address` plus this, so fold the two
        // together: the job carries the entry list's own address.
        job.address = gpfifo + byte_offset;
        job.size = byte_size;
        job.gpfifo = true;
        job.on_done = std::move(on_done);
        queue_.push_back(std::move(job));
    }
    queue_ready_.notify_one();
}

SubmitResult Gpu::Run(const Job& job) {
    if (job.gpfifo) {
        // The address already has the offset folded in, so run from zero.
        return SubmitGpfifo(job.channel, job.address, 0, job.size);
    }
    return SubmitPushbuffer(job.channel, job.address, job.size);
}

void Gpu::WaitForIdle() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

void Gpu::RenderWorker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;  // stopping, and nothing left to drain
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }

        // The same routines a synchronous caller uses, so both paths render
        // identically and both publish before saying they are done.
        const SubmitResult result = Run(job);
        if (job.on_done) {
            job.on_done(result);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            --running_;
            if (queue_.empty() && running_ == 0) {
                queue_idle_.notify_all();
            }
        }
    }
}

void Gpu::ReportFault(GpuFaultKind kind, GuestPa address, std::uint32_t method,
                      const std::string& detail) {
    GpuFault fault;
    fault.kind = kind;
    fault.address = address;
    fault.method = method;
    fault.detail = detail;
    host_.OnGpuFault(fault);
}

bool Gpu::ExecuteDraw(const render::DrawDesc& draw, SubmitResult& result) {
    if (backend_ == nullptr) {
        result.status = SubmitStatus::NoBackend;
        result.detail = "a draw was assembled but no render backend is installed";
        ReportFault(GpuFaultKind::Internal, draw.color.pa, 0, result.detail);
        return false;
    }

    // Ask before running: a backend that cannot represent this state should say
    // so, rather than execute it and put wrong pixels in guest memory.
    std::string why;
    if (!backend_->Supports(draw, why)) {
        result.status = SubmitStatus::RenderFailed;
        result.detail = why;
        ReportFault(GpuFaultKind::UnimplementedMethod, draw.color.pa, 0, why);
        return false;
    }

    const render::RenderResult rendered = backend_->Execute(draw);
    if (!rendered.ok()) {
        result.status = SubmitStatus::RenderFailed;
        result.detail = rendered.detail;
        ReportFault(rendered.status == render::RenderStatus::DeviceLost ? GpuFaultKind::DeviceLost
                                                                       : GpuFaultKind::Internal,
                    draw.color.pa, 0, rendered.detail);
        return false;
    }

    ++result.draws;
    return true;
}

SubmitResult Gpu::SubmitPushbuffer(ChannelId channel, GuestPa address, std::uint64_t size) {
    SubmitResult result;

    // A stream is decoded against ONE channel's register file. Two channels must not
    // share it: the colour target, viewport, depth state, selected shader and pending
    // draw all belong to the channel that set them.
    Channel* state = FindChannel(channel);
    if (state == nullptr) {
        result.status = SubmitStatus::NoChannel;
        result.detail = "no such channel";
        return result;
    }
    engine::Maxwell3D& engine = state->engine;

    // Nothing to run is not a fault: a driver submits an empty buffer to mean
    // "nothing this time".
    if (size == 0) {
        return result;
    }

    const std::uint8_t* bytes = memory_.peek(address, size);
    if (bytes == nullptr) {
        result.status = SubmitStatus::BufferNotMapped;
        result.detail = "the command buffer is not mapped guest memory";
        ReportFault(GpuFaultKind::AddressTranslation, address, 0, result.detail);
        return result;
    }

    // Decoding a stream and running it are different costs, so they are counted apart.
    profile_ = Profile{};
    const auto probe = [] { return std::chrono::steady_clock::now(); };
    const auto since = [](auto started) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         started)
            .count();
    };

    std::string error;
    const bool walked = engine::PushBufferReader::WalkMemory(
        bytes, size,
        [&](const engine::Method& method, const std::uint32_t* arguments) {
            ++result.methods;

            const auto decode_started = probe();
            const bool dispatched = engine.Dispatch(method, arguments, error);
            profile_.decode_ms += since(decode_started);
            if (!dispatched) {
                // A method the engine needed was malformed. The reader treats a
                // false emit as "done", so the walk stops here and the reason is
                // kept in `result` -- the status below is deliberately sticky.
                result.status = SubmitStatus::MalformedMethod;
                result.detail = error;
                ReportFault(GpuFaultKind::UnimplementedMethod, address, method.address, error);
                return false;
            }

            // One pending draw at a time: take it before the next method can
            // overwrite it, which is what a stream with several draws needs.
            if (engine.HasDraw()) {
                const auto execute_started = probe();
                const bool drawn = ExecuteDraw(*engine.TakeDraw(), result);
                profile_.execute_ms += since(execute_started);
                return drawn;
            }
            return true;
        },
        error);

    if (!walked) {
        // The reader refused the stream itself, which is distinct from a method
        // the engine refused: that path returns true with the status already set.
        result.status = SubmitStatus::MalformedStream;
        result.detail = error;
        ReportFault(GpuFaultKind::MalformedCommandStream, address, 0, error);
        return result;
    }

    if (result.ok() && result.draws != 0) {
        // The guest expects the pixels to be there when the submission returns,
        // so this is where an asynchronous backend is made to catch up.
        std::string why;
        const auto flush_started = probe();
        const bool flushed = backend_->Flush(why);
        profile_.flush_ms += since(flush_started);
        if (!flushed) {
            result.status = SubmitStatus::RenderFailed;
            result.detail = why;
            ReportFault(GpuFaultKind::Internal, address, 0, why);
        }
    }

    return result;
}

}  // namespace zlong::gpu
