// 烛龙 (ZhuLong) - a guest process.
//
// A process owns a handle table and the view of the mounted sources its threads
// resolve paths through. It owns no memory: the address space belongs to the RAM
// layer, and the process only refers to it -- which is what keeps this layer
// free of page-table knowledge.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "zlong/mount/mount.h"
#include "zlong/service/handle.h"
#include "zlong/service/object.h"

namespace zlong::service {

class KAddressSpace;
class KThread;

class KProcess final : public KObject {
public:
    static constexpr ObjectKind kKind = ObjectKind::Process;

    explicit KProcess(std::uint64_t pid) noexcept;

    std::uint64_t pid() const noexcept { return pid_; }

    HandleTable& handles() noexcept { return handles_; }
    const HandleTable& handles() const noexcept { return handles_; }

    /// Install the mounted sources. Set once, before the process runs, and never
    /// changed afterwards -- so readers need no lock for it.
    void set_mounts(const mount::MountTable* mounts) noexcept { mounts_ = mounts; }
    const mount::MountTable* mounts() const noexcept { return mounts_; }

    /// The address space the process's threads run in. Set once at boot, and
    /// never changed: under HLE the guest's own EL1 kernel does not run, so there
    /// is one regime and nothing switches it.
    ///
    /// Borrowed: the kernel owns the address space and outlives any process. The
    /// process only says which one it runs in, which is what keeps this layer
    /// free of page-table knowledge.
    void set_address_space(KAddressSpace* space) noexcept { address_space_ = space; }
    KAddressSpace* address_space() const noexcept { return address_space_; }

    /// The thread the process starts on. Set once at boot.
    ///
    /// Borrowed on the same contract as the kernel's waiter registry: the owner
    /// keeps it alive and has to unbind before it dies. Nothing here takes a
    /// reference, because a thread that outlived its process would be a worse
    /// lie than a documented contract.
    void set_main_thread(KThread* thread) noexcept { main_thread_ = thread; }
    KThread* main_thread() const noexcept { return main_thread_; }

    bool terminated() const noexcept { return terminated_.load(std::memory_order_acquire); }

    /// Mark the process dead and close every handle it holds. Idempotent: the
    /// first call wins and the rest are no-ops.
    void Terminate() noexcept;

private:
    const std::uint64_t pid_;
    HandleTable handles_;
    const mount::MountTable* mounts_ = nullptr;
    KAddressSpace* address_space_ = nullptr;
    KThread* main_thread_ = nullptr;
    std::atomic<bool> terminated_{false};
    mutable std::mutex terminate_mutex_;
};

}  // namespace zlong::service
