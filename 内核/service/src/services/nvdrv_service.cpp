#include "zlong/service/services/nvdrv_service.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "../word_io.h"
#include "zlong/cpu/core.h"
#include "zlong/gpu/gpu.h"

namespace zlong::service {

using word_io::PutU32;
using word_io::Words;

namespace {

/// The devices that accept channel ioctls. Deliberately loose: the console's
/// per-node ioctl matrix is not transcribed, and guessing it would be worse than
/// admitting that /dev/nvmap is the memory mapper and the nvhost nodes are not.
bool AcceptsChannels(const std::string& name) {
    return name == kNvHostGpu || name == kNvHostCtrlGpu || name == kNvHostCtrl;
}

}  // namespace

void NvDrvService::HandleRequest(ServiceContext& context, const IpcRequest& request,
                                 IpcResponse& response) {
    switch (request.command_id) {
    case kCommandOpen:
        Open(request, response);
        return;
    case kCommandClose:
        Close(request, response);
        return;
    case kCommandIoctl:
        RunIoctl(context, request, response);
        return;
    default:
        ++unknown_commands_;
        response.result = kIpcResultUnknownCommand;
        return;
    }
}

void NvDrvService::Open(const IpcRequest& request, IpcResponse& response) {
    const std::string name = word_io::Text(request.data);
    if (name.empty()) {
        Refuse(response, kNvDrvResultBadArgument, "the device name is empty");
        return;
    }
    if (name != kNvHostCtrl && name != kNvHostCtrlGpu && name != kNvHostGpu && name != kNvMap) {
        Refuse(response, kNvDrvResultNoSuchDevice, "no such device: " + name);
        return;
    }

    const std::uint32_t fd = next_fd_++;
    devices_[fd] = Device{name};
    ++opens_;
    last_error_.clear();
    response.result = 0;
    PutU32(response.data, fd);
}

void NvDrvService::Close(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    if (!words.U32(fd)) {
        Refuse(response, kNvDrvResultBadArgument, "close needs an fd");
        return;
    }

    const auto device = devices_.find(fd);
    if (device == devices_.end()) {
        Refuse(response, kNvDrvResultNoSuchDevice, "that fd is not open");
        return;
    }

    // Closing the device closes the channels opened through it, so a guest that
    // leaks its channels cannot keep their syncpoints alive.
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second.owner == fd) {
            syncpoints_.erase(it->second.syncpoint);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    devices_.erase(device);

    ++closes_;
    last_error_.clear();
    response.result = 0;
}

void NvDrvService::RunIoctl(ServiceContext& context, const IpcRequest& request,
                            IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    if (!words.U32(fd) || !words.U32(ioctl)) {
        Refuse(response, kNvDrvResultBadArgument, "an ioctl needs an fd and a number");
        return;
    }

    const auto device = devices_.find(fd);
    if (device == devices_.end()) {
        Refuse(response, kNvDrvResultNoSuchDevice, "that fd is not open");
        return;
    }
    // The node decides which ioctls exist. /dev/nvmap is the memory mapper and the
    // nvhost nodes take channel ioctls; keeping them apart is the beginning of the
    // per-node matrix the header admits is not transcribed, and a mapper that accepted
    // channel ioctls would only hide that.
    if (device->second.name == kNvMap) {
        RunNvMapIoctl(request, response);
        return;
    }
    if (!AcceptsChannels(device->second.name)) {
        Refuse(response, kNvDrvResultWrongDevice,
               device->second.name + " does not take channel ioctls");
        return;
    }

    // Each handler re-reads the words itself, so a missing argument is reported
    // by the ioctl that needed it rather than by the dispatcher.
    switch (ioctl) {
    case kIoctlChannelOpen:
        OpenChannel(fd, response);
        return;
    case kIoctlChannelBindGpfifo:
        BindGpfifo(request, response);
        return;
    case kIoctlChannelSubmitGpfifo:
        SubmitGpfifo(request, response);
        return;
    case kIoctlChannelClose:
        CloseChannel(request, response);
        return;
    case kIoctlChannelWaitSyncpoint:
        WaitSyncpoint(context, request, response);
        return;
    default:
        ++unknown_ioctls_;
        Refuse(response, kNvDrvResultBadArgument, "unknown ioctl");
        return;
    }
}

void NvDrvService::OpenChannel(std::uint32_t device_fd, IpcResponse& response) {
    const std::uint32_t fd = next_fd_++;
    const gpu::SyncpointId syncpoint = next_syncpoint_++;
    Channel channel;
    channel.owner = device_fd;
    channel.syncpoint = syncpoint;
    // A channel is a register file as well as a queue: give it its own, so this
    // channel's colour target, viewport and shaders are nobody else's.
    if (gpu_ != nullptr) {
        channel.gpu_channel = gpu_->CreateChannel();
    }
    channels_[fd] = channel;
    // Record the syncpoint at zero, so "allocated and never advanced" is
    // distinguishable from "never allocated".
    syncpoints_[syncpoint] = 0;

    last_error_.clear();
    response.result = 0;
    PutU32(response.data, fd);
    PutU32(response.data, syncpoint);
}

void NvDrvService::BindGpfifo(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t channel_fd = 0;
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(channel_fd) || !words.U64(address) ||
        !words.U32(size)) {
        Refuse(response, kNvDrvResultBadArgument, "bind gpfifo needs a channel, an address, a size");
        return;
    }
    if (size == 0) {
        Refuse(response, kNvDrvResultBadArgument, "the gpfifo buffer has no size");
        return;
    }
    Channel* channel = FindChannel(channel_fd);
    if (channel == nullptr) {
        Refuse(response, kNvDrvResultNoSuchChannel, "that fd is not an open channel");
        return;
    }

    channel->gpfifo_name = address;
    channel->gpfifo_size = size;

    if (gpu_ == nullptr) {
        // Nothing to resolve through yet. Binding is bookkeeping; a submit with no GPU
        // attached is refused before this address is ever used, which is the behaviour
        // the caller already relies on.
        channel->gpfifo = 0;
        last_error_.clear();
        response.result = 0;
        return;
    }

    // The address is a GPU address, like every other address the device is handed: the
    // channel reads its GPFIFO through the address space rather than out of physical
    // memory. Resolving HERE rather than at first submit is what lets a GPFIFO the guest
    // never mapped say so at bind time.
    const std::optional<gpu::GuestPa> physical = gpu_->address_space().Resolve(address, size);
    if (!physical.has_value()) {
        Refuse(response, kNvDrvResultBadArgument,
               "the gpfifo names GPU address " + std::to_string(address) +
                   ", which was never mapped");
        return;
    }

    channel->gpfifo = *physical;
    last_error_.clear();
    response.result = 0;
}

void NvDrvService::SubmitGpfifo(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t channel_fd = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t increment = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(channel_fd) || !words.U32(offset) ||
        !words.U32(size) || !words.U32(increment)) {
        Refuse(response, kNvDrvResultBadArgument, "submit needs a channel, an offset, a size, a bump");
        return;
    }
    if (size == 0) {
        Refuse(response, kNvDrvResultBadArgument, "the submission has no size");
        return;
    }
    if (increment == 0) {
        Refuse(response, kNvDrvResultBadArgument, "the syncpoint increment is zero");
        return;
    }

    Channel* channel = FindChannel(channel_fd);
    if (channel == nullptr) {
        Refuse(response, kNvDrvResultNoSuchChannel, "that fd is not an open channel");
        return;
    }
    // No device is a more fundamental refusal than an unbound buffer, and it is the one
    // a caller with no GPU can do something about.
    if (gpu_ == nullptr) {
        Refuse(response, kNvDrvResultNoGpu, "no GPU is attached");
        return;
    }
    if (channel->gpfifo == 0) {
        Refuse(response, kNvDrvResultBadArgument, "the channel has no gpfifo buffer bound");
        return;
    }
    if (static_cast<std::uint64_t>(offset) + size > channel->gpfifo_size) {
        Refuse(response, kNvDrvResultBadArgument, "the submission runs past the gpfifo buffer");
        return;
    }
    // Hand the stream to the render worker and return the fence value the guest will
    // wait on. The value is known now, which is the point of a fence: the guest does
    // not need the work to be finished to know what to wait for, and this core goes
    // straight back to running guest code instead of spending itself on the render.
    const gpu::SyncpointId syncpoint = channel->syncpoint;
    std::uint32_t value = 0;
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        value = syncpoints_[syncpoint] + increment;
        syncpoints_[syncpoint] = value;
    }

    // A stream the GPU refuses is only found out on the worker now, which is what the
    // console does too: the ioctl accepts the buffer and the fault surfaces
    // afterwards, through the GPU host. The fence still arrives either way, because a
    // guest blocked on a value that never comes is worse off than a guest holding a
    // frame it can tell is wrong.
    // The submitted range is a GPFIFO: entries, each naming a command buffer. The
    // driver submits through this layer, not through one flat command stream.
    gpu_->SubmitGpfifoAsync(
        channel->gpu_channel, channel->gpfifo, offset, size,
        [this, syncpoint, value](const gpu::SubmitResult&) { AdvanceSyncpoint(syncpoint, value); });

    ++submits_;
    last_error_.clear();
    response.result = 0;
    PutU32(response.data, value);
}

/// Block the calling core until a syncpoint reaches a value.
///
/// Registering before parking is what makes a signal that arrives in between
/// impossible to lose: the waiter is on the list before the core stops, and the
/// signal drains the list and unparks. A woken core re-checks rather than assuming
/// its value arrived, so a spurious wake costs one more park and nothing else.
void NvDrvService::WaitSyncpoint(ServiceContext& context, const IpcRequest& request,
                                 IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t id = 0;
    std::uint32_t value = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(id) || !words.U32(value)) {
        Refuse(response, kNvDrvResultBadArgument, "a wait needs a syncpoint and a value");
        return;
    }

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(sync_mutex_);
            const auto reached = reached_.find(id);
            if (reached != reached_.end() && reached->second >= value) {
                break;
            }
            std::vector<cpu::Core*>& waiters = sync_waiters_[id];
            if (std::find(waiters.begin(), waiters.end(), &context.core) == waiters.end()) {
                waiters.push_back(&context.core);
            }
        }
        context.core.ParkCurrentThread();
    }

    last_error_.clear();
    response.result = 0;
}

void NvDrvService::AdvanceSyncpoint(gpu::SyncpointId id, std::uint32_t value) {
    // The kernel is told *before* the value is published, so a core that can see its
    // fence reached also knows the signal that came with it has already been sent: the
    // waiter re-checks the value under this lock, so the lock's release here orders the
    // notification before anything that observes the value.
    //
    // The other order left a window -- publish first, notify second -- in which a waiter
    // could take the lock on a round of its park loop, see the value, and return before
    // the notification had happened. That window is what made
    // the_submission_renders_by_the_time_its_syncpoint_is_reached flaky, with the
    // startling symptom of an assertion reporting (got 1, want 1): the comparison read
    // the counter before the worker had written it and the failure message, a moment
    // later, read it after.
    if (gpu_ != nullptr) {
        gpu_->host().OnSyncpointSignal(id, value);
    }

    std::vector<cpu::Core*> wake;
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        std::uint32_t& reached = reached_[id];
        if (value > reached) {
            reached = value;
        }
        const auto waiting = sync_waiters_.find(id);
        if (waiting != sync_waiters_.end()) {
            wake.swap(waiting->second);
        }
    }

    // Unpark outside the lock: a woken core takes the lock again immediately to
    // re-check, so holding it here would hand the lock from one to the other for no
    // reason.
    for (cpu::Core* core : wake) {
        core->Unpark();
    }
}

void NvDrvService::CloseChannel(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t channel_fd = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(channel_fd)) {
        Refuse(response, kNvDrvResultBadArgument, "close channel needs a channel");
        return;
    }
    const auto channel = channels_.find(channel_fd);
    if (channel == channels_.end()) {
        Refuse(response, kNvDrvResultNoSuchChannel, "that fd is not an open channel");
        return;
    }
    syncpoints_.erase(channel->second.syncpoint);
    if (gpu_ != nullptr && channel->second.gpu_channel != gpu::kInvalidChannel) {
        gpu_->DestroyChannel(channel->second.gpu_channel);
    }
    channels_.erase(channel);

    last_error_.clear();
    response.result = 0;
}

bool NvDrvService::Refuse(IpcResponse& response, std::uint32_t result, const std::string& why) {
    response.result = result;
    response.data.clear();
    last_error_ = why;
    return false;
}

NvDrvService::Channel* NvDrvService::FindChannel(std::uint32_t fd) {
    const auto found = channels_.find(fd);
    return found == channels_.end() ? nullptr : &found->second;
}

void NvDrvService::RunNvMapIoctl(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    if (!words.U32(fd) || !words.U32(ioctl)) {
        Refuse(response, kNvDrvResultBadArgument, "an ioctl needs an fd and a number");
        return;
    }

    switch (ioctl) {
    case kIoctlNvMapAllocate:
        AllocateMemory(request, response);
        return;
    case kIoctlNvMapFree:
        FreeMemory(request, response);
        return;
    case kIoctlNvMapExposeForGpu:
        ExposeForGpu(request, response);
        return;
    default:
        ++unknown_ioctls_;
        Refuse(response, kNvDrvResultBadArgument, "unknown nvmap ioctl");
        return;
    }
}

void NvDrvService::AllocateMemory(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(size) || !words.U32(alignment)) {
        Refuse(response, kNvDrvResultBadArgument, "an allocation needs a size and an alignment");
        return;
    }
    if (size == 0) {
        Refuse(response, kNvDrvResultBadArgument, "the allocation has no size");
        return;
    }
    if (alignment != 0 && (alignment & (alignment - 1)) != 0) {
        Refuse(response, kNvDrvResultBadArgument, "the alignment is not a power of two");
        return;
    }
    if (gpu_ == nullptr) {
        Refuse(response, kNvDrvResultNoGpu, "no GPU is attached");
        return;
    }

    // The backing store is guest physical memory. On a unified-memory machine that IS
    // what video memory is, so this reserves a range rather than a second pool -- and
    // what the guest gets back is a handle, not an address, because the handle is what
    // the rest of the driver passes around.
    const auto physical =
        gpu_->physical().allocate(size, alignment == 0 ? 0x1000 : alignment);
    if (!physical.has_value()) {
        Refuse(response, kNvDrvResultBadArgument, "guest physical memory is exhausted");
        return;
    }

    const std::uint32_t handle = next_nvmap_handle_++;
    NvMapAllocation allocation;
    allocation.physical = *physical;
    allocation.size = size;
    nvmap_[handle] = allocation;

    last_error_.clear();
    response.result = 0;
    PutU32(response.data, handle);
}

void NvDrvService::FreeMemory(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t handle = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(handle)) {
        Refuse(response, kNvDrvResultBadArgument, "a free needs a handle");
        return;
    }
    const auto found = nvmap_.find(handle);
    if (found == nvmap_.end()) {
        Refuse(response, kNvDrvResultBadArgument, "that handle is not an allocation");
        return;
    }

    if (found->second.exposed && gpu_ != nullptr) {
        gpu_->address_space().Unmap(found->second.gpu_address);
    }
    nvmap_.erase(found);
    // The physical range is NOT returned to the allocator: it is a bump allocator, so
    // a guest that allocates and frees in a loop runs it down. Said here rather than
    // left to be discovered.
    last_error_.clear();
    response.result = 0;
}

void NvDrvService::ExposeForGpu(const IpcRequest& request, IpcResponse& response) {
    Words words(request.data);
    std::uint32_t fd = 0;
    std::uint32_t ioctl = 0;
    std::uint32_t handle = 0;
    if (!words.U32(fd) || !words.U32(ioctl) || !words.U32(handle)) {
        Refuse(response, kNvDrvResultBadArgument, "exposing needs a handle");
        return;
    }
    const auto found = nvmap_.find(handle);
    if (found == nvmap_.end()) {
        Refuse(response, kNvDrvResultBadArgument, "that handle is not an allocation");
        return;
    }
    if (gpu_ == nullptr) {
        Refuse(response, kNvDrvResultNoGpu, "no GPU is attached");
        return;
    }

    NvMapAllocation& allocation = found->second;
    if (!allocation.exposed) {
        allocation.gpu_address = gpu_->address_space().Map(allocation.physical, allocation.size);
        allocation.exposed = true;
    }

    last_error_.clear();
    response.result = 0;
    PutU32(response.data, static_cast<std::uint32_t>(allocation.gpu_address));
    PutU32(response.data, static_cast<std::uint32_t>(allocation.gpu_address >> 32));
}

std::optional<NvDrvService::NvMapAllocation> NvDrvService::nvmap_allocation(
    std::uint32_t handle) const {
    const auto found = nvmap_.find(handle);
    if (found == nvmap_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::uint32_t NvDrvService::syncpoint_value(gpu::SyncpointId id) const {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    const auto found = syncpoints_.find(id);
    return found == syncpoints_.end() ? 0 : found->second;
}

std::uint32_t NvDrvService::syncpoint_reached(gpu::SyncpointId id) const {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    const auto found = reached_.find(id);
    return found == reached_.end() ? 0 : found->second;
}

}  // namespace zlong::service
