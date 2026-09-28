// 烛龙 (ZhuLong) - a guest thread.
//
// A thread is the unit that blocks. Its WaiterId IS its tid, so a woken waiter
// is resolved by looking the tid up -- no separate registry of waiter ids, and
// no way for the two to disagree.
//
// The state machine is owned by the kernel: this type records a transition and
// reports it, and never decides one.

#pragma once

#include <cstdint>
#include <mutex>

#include "zlong/service/object.h"
#include "zlong/service/sync.h"

namespace zlong::cpu {
class Core;
}

namespace zlong::service {

class KThread final : public KObject {
public:
    static constexpr ObjectKind kKind = ObjectKind::Thread;

    enum class State : std::uint8_t {
        /// Created but not started.
        Created,
        /// Runnable.
        Ready,
        /// On a core right now.
        Running,
        /// Blocked on something, and registered with it.
        Waiting,
        /// Finished.
        Terminated,
    };

    KThread(std::uint64_t tid, std::uint64_t owner_pid) noexcept;

    std::uint64_t tid() const noexcept { return tid_; }
    std::uint64_t owner_pid() const noexcept { return owner_pid_; }

    /// What a waitable object knows this thread by.
    WaiterId waiter_id() const noexcept { return tid_; }

    State state() const;
    void set_state(State state);

    /// Where the thread begins, and what it begins with. Recorded at creation
    /// and consumed when the core is started -- there is no context switching
    /// yet, so a thread cannot start itself.
    void set_start(std::uint64_t entry, std::uint64_t stack, std::uint64_t argument);
    std::uint64_t entry() const;
    std::uint64_t stack() const;
    std::uint64_t argument() const;

    /// The core this thread is bound to, or nullptr. Bound by the kernel when
    /// the thread starts.
    ///
    /// A pointer rather than an index on purpose: waking a waiter has to reach
    /// the core, and holding it directly means that path never needs a Cpu& to
    /// look one up.
    cpu::Core* core() const;
    void set_core(cpu::Core* core);
    bool has_core() const;

private:
    const std::uint64_t tid_;
    const std::uint64_t owner_pid_;
    mutable std::mutex mutex_;
    State state_ = State::Created;
    cpu::Core* core_ = nullptr;
    std::uint64_t entry_ = 0;
    std::uint64_t stack_ = 0;
    std::uint64_t argument_ = 0;
};

const char* ToString(KThread::State state) noexcept;

}  // namespace zlong::service
