// 烛龙 (ZhuLong) - nvdrv: the guest's path to the GPU.
//
// The console's real driver is a set of /dev/nvhost-* nodes and a pile of _IOWR
// ioctls. Two things below are THIS PROJECT'S PLACEHOLDER, stated rather than
// left to be inferred:
//
//   * the command ids, like every other IPC command id in this layer;
//   * the ioctl numbers, and the shape of a request's data (a word for the fd, a
//     word for the ioctl, then that ioctl's own words). The console's request
//     structs, and the device/ioctl matrix that says which ioctl each node
//     accepts, are not transcribed.
//
// What is NOT a placeholder is the shape of the path: open a device, open a
// channel on it, give the channel a command buffer, submit it, and have the
// submission advance a syncpoint. Every one of those steps can refuse, and says
// why rather than succeeding quietly.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "zlong/gpu/gpu_host.h"
#include "zlong/gpu/types.h"
#include "zlong/service/registry.h"

namespace zlong::gpu {
class Gpu;
}

namespace zlong::service {

inline constexpr const char* kNvHostCtrl = "/dev/nvhost-ctrl";
inline constexpr const char* kNvHostCtrlGpu = "/dev/nvhost-ctrl-gpu";
inline constexpr const char* kNvHostGpu = "/dev/nvhost-gpu";
inline constexpr const char* kNvMap = "/dev/nvmap";

/// The first fd handed out. Zero is never a valid fd here, so a guest that
/// forgot to read the out-parameter cannot look like it opened something.
inline constexpr std::uint32_t kNvDrvFirstFd = 1;

/// Failure results. Nonzero, in the same placeholder family as the syscall and
/// IPC results.
enum : std::uint32_t {
    kNvDrvResultBadArgument = 0x5A4E'0001u,
    kNvDrvResultNoSuchDevice = 0x5A4E'0002u,
    kNvDrvResultNoSuchChannel = 0x5A4E'0003u,
    kNvDrvResultNoGpu = 0x5A4E'0004u,
    kNvDrvResultSubmitFailed = 0x5A4E'0005u,
    kNvDrvResultWrongDevice = 0x5A4E'0006u,
};

class NvDrvService final : public IService {
public:
    /// Commands, in the guest's command buffer.
    enum Command : std::uint32_t {
        /// data: the device name. response: u32 fd.
        kCommandOpen = 1,
        /// data: u32 fd. response: none.
        kCommandClose = 2,
        /// data: u32 fd, u32 ioctl, then that ioctl's words.
        /// response: that ioctl's words.
        kCommandIoctl = 3,
    };

    /// Ioctls this service implements. Placeholder numbers.
    enum Ioctl : std::uint32_t {
        /// words: none. response: u32 channel fd, u32 syncpoint id.
        kIoctlChannelOpen = 0x0001,
        /// words: u32 channel fd, u64 gpfifo address, u32 size. response: none.
        kIoctlChannelBindGpfifo = 0x0002,
        /// words: u32 channel fd, u32 offset, u32 size, u32 increment.
        /// response: u32 syncpoint value after the submission.
        ///
        /// The offset and size are a byte range inside the channel's GPFIFO buffer,
        /// and that range is a list of GPFIFO entries -- each naming the command
        /// buffer it stands for. The console submits a count of entries rather than a
        /// byte size; that is the one difference left.
        kIoctlChannelSubmitGpfifo = 0x0003,
        /// words: u32 channel fd. response: none.
        kIoctlChannelClose = 0x0004,
        /// words: u32 syncpoint id, u32 value. response: none.
        ///
        /// Blocks the calling core until that syncpoint reaches that value. This is
        /// the other half of an asynchronous submission: without it a guest has no
        /// way to know the frame it asked for is finished.
        kIoctlChannelWaitSyncpoint = 0x0005,
    };

    /// Ioctls /dev/nvmap takes. Placeholder numbers, in the same family as the
    /// channel ones, and deliberately separate from them: the console's per-node
    /// ioctl matrix is not transcribed, and a mapper that accepted channel ioctls
    /// would only hide that.
    enum NvMapIoctl : std::uint32_t {
        /// words: u32 size, u32 alignment. response: u32 handle.
        kIoctlNvMapAllocate = 0x0001,
        /// words: u32 handle. response: none.
        kIoctlNvMapFree = 0x0002,
        /// words: u32 handle. response: u32 gpu address (low), u32 (high).
        ///
        /// Names the allocation in the GPU's address space. Until this has been done
        /// the memory exists but nothing the GPU is told about can point at it.
        kIoctlNvMapExposeForGpu = 0x0003,
    };

    const char* name() const noexcept override { return "nvdrv"; }

    /// Wire the submission path in. Borrowed, and may be null: without it a
    /// submission is refused with a reason instead of quietly succeeding. The
    /// syncpoint signal goes to the Gpu's own host, so this is the same object
    /// that receives the GPU's faults.
    void set_gpu(gpu::Gpu* gpu) noexcept { gpu_ = gpu; }
    gpu::Gpu* gpu() const noexcept { return gpu_; }

    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;

    // --- what it has served, for tests and diagnosis ------------------------
    std::uint32_t opens() const noexcept { return opens_; }
    std::uint32_t closes() const noexcept { return closes_; }
    std::uint32_t submits() const noexcept { return submits_; }
    std::uint32_t unknown_commands() const noexcept { return unknown_commands_; }
    std::uint32_t unknown_ioctls() const noexcept { return unknown_ioctls_; }

    /// Live device fds.
    std::size_t device_count() const noexcept { return devices_.size(); }
    /// Live channels.
    std::size_t channel_count() const noexcept { return channels_.size(); }
    /// Live nvmap allocations.
    std::size_t nvmap_allocation_count() const noexcept { return nvmap_.size(); }

    /// What an nvmap handle names, for tests and diagnosis. `physical` is what the
    /// guest never has to see; `gpu_address` is what it does.
    struct NvMapAllocation {
        gpu::GuestPa physical = 0;
        std::uint64_t size = 0;
        gpu::GpuVAddr gpu_address = 0;
        bool exposed = false;
    };
    std::optional<NvMapAllocation> nvmap_allocation(std::uint32_t handle) const;

    /// The current value of a syncpoint, or 0 when it was never allocated. A
    /// syncpoint that exists and has never advanced is also 0; the id being
    /// allocated is what `channels()` says.
    std::uint32_t syncpoint_value(gpu::SyncpointId id) const;

    /// What the GPU has actually finished. It lags `syncpoint_value` while a
    /// submission is in flight, which is the whole reason a guest has to wait.
    std::uint32_t syncpoint_reached(gpu::SyncpointId id) const;

    /// The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }

private:
    struct Device {
        std::string name;
    };

    struct Channel {
        /// The device fd the channel was opened through, so closing the device
        /// closes its channels.
        std::uint32_t owner = 0;
        /// The GPFIFO buffer the guest bound, as the physical memory its GPU address
        /// resolved to. The channel reads its entries out of this.
        gpu::GuestPa gpfifo = 0;
        /// The GPU address the guest bound, kept so a refusal can name what it said.
        gpu::GpuVAddr gpfifo_name = 0;
        std::uint64_t gpfifo_size = 0;
        gpu::SyncpointId syncpoint = gpu::kInvalidSyncpoint;
        /// The GPU-side register file this channel submits against. Each channel
        /// gets its own, because two channels must not see each other's engine
        /// state.
        gpu::ChannelId gpu_channel = gpu::kInvalidChannel;
    };

    void Open(const IpcRequest& request, IpcResponse& response);
    void Close(const IpcRequest& request, IpcResponse& response);
    void RunIoctl(ServiceContext& context, const IpcRequest& request, IpcResponse& response);

    void OpenChannel(std::uint32_t device_fd, IpcResponse& response);
    void BindGpfifo(const IpcRequest& request, IpcResponse& response);
    void SubmitGpfifo(const IpcRequest& request, IpcResponse& response);
    void CloseChannel(const IpcRequest& request, IpcResponse& response);
    void WaitSyncpoint(ServiceContext& context, const IpcRequest& request, IpcResponse& response);

    void RunNvMapIoctl(const IpcRequest& request, IpcResponse& response);
    void AllocateMemory(const IpcRequest& request, IpcResponse& response);
    void FreeMemory(const IpcRequest& request, IpcResponse& response);
    void ExposeForGpu(const IpcRequest& request, IpcResponse& response);

    /// Record that a syncpoint has reached `value`, and wake every core waiting for
    /// it. Called on the render worker, so it may not touch anything that belongs to
    /// the submitting core.
    void AdvanceSyncpoint(gpu::SyncpointId id, std::uint32_t value);

    /// Set a failure result and its reason, and report the refusal to the caller
    /// by returning false.
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);

    /// The channel a word in a request names, or null when the fd is not an open
    /// channel.
    Channel* FindChannel(std::uint32_t fd);

    gpu::Gpu* gpu_ = nullptr;

    std::map<std::uint32_t, Device> devices_;
    std::map<std::uint32_t, Channel> channels_;
    std::map<gpu::SyncpointId, std::uint32_t> syncpoints_;
    std::uint32_t next_fd_ = kNvDrvFirstFd;
    gpu::SyncpointId next_syncpoint_ = 1;

    /// Syncpoint state, shared with the render worker. `syncpoints_` is the value
    /// the guest was handed at submit time; `reached_` is what the GPU has actually
    /// finished. They differ while work is in flight, which is exactly what the wait
    /// is for.
    mutable std::mutex sync_mutex_;
    std::map<gpu::SyncpointId, std::uint32_t> reached_;
    std::map<gpu::SyncpointId, std::vector<cpu::Core*>> sync_waiters_;

    /// nvmap: what a handle names. The backing store is guest physical memory -- the
    /// console has no separate VRAM -- and `gpu_address` is filled in when the guest
    /// asks for the allocation to be usable by the GPU.
    std::map<std::uint32_t, NvMapAllocation> nvmap_;
    std::uint32_t next_nvmap_handle_ = 1;

    std::uint32_t opens_ = 0;
    std::uint32_t closes_ = 0;
    std::uint32_t submits_ = 0;
    std::uint32_t unknown_commands_ = 0;
    std::uint32_t unknown_ioctls_ = 0;
    std::string last_error_;
};

}  // namespace zlong::service
