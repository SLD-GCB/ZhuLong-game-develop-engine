// 烛龙 (ZhuLong) - the waitable kernel objects.
//
// Every one of them obeys the same three rules. They are what make waiting safe
// rather than mostly-safe, and each rule exists because of a specific way the
// obvious version goes wrong:
//
//   1. PrepareWait / PrepareLock REGISTERS the waiter and then reports whether
//      the caller must park at all. Registering first is what makes a signal
//      arriving in between impossible to lose. Parking first and registering
//      after drops that signal on the floor and strands the waiter forever.
//
//   2. A signal does NOT wake anybody. Signal / Release return the waiters that
//      were released, and the caller unparks them *after* dropping this object's
//      lock. That keeps the global lock order object.mutex -> core.mutex and
//      means no unpark happens while an object lock is held.
//
//   3. Every wait is a retry loop: a woken waiter re-runs the operation it was
//      doing from the top. Nothing here hands over ownership on wake, so a wait
//      that is cancelled can never leave its waiter holding something it does
//      not know about.
//
// The objects do not know what a core is. A waiter is an opaque id; the layer
// above maps it to a cpu::Core and does the parking.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "zlong/service/object.h"

namespace zlong::service {

/// Who to wake. Opaque here on purpose: this layer never parks anything.
using WaiterId = std::uint64_t;

/// A manual/auto event.
class KEvent final : public KObject {
public:
    static constexpr ObjectKind kKind = ObjectKind::Event;

    enum class Mode : std::uint8_t {
        /// Stays signalled until Clear; releases everyone waiting.
        Manual,
        /// Signalling releases one waiter, or is remembered if nobody is waiting.
        Auto,
    };

    KEvent(Mode mode, bool initially_signalled);

    /// Register `waiter`. Returns true when it must park. False means the event
    /// is already signalled -- and for an auto event this call consumed that
    /// signal -- so the caller must proceed, not park.
    bool PrepareWait(WaiterId waiter);

    /// Drop a registration. False when it was not registered.
    bool CancelWait(WaiterId waiter);

    /// Release the waiters this signal wakes. A manual event wakes everyone and
    /// stays signalled; an auto event wakes one, or sets the signal when nobody
    /// is waiting.
    std::vector<WaiterId> Signal();

    /// Clear a manual event's signal. An auto event clears itself when consumed.
    void Clear();

    bool signalled() const;
    Mode mode() const noexcept;
    std::size_t waiter_count() const;

private:
    mutable std::mutex mutex_;
    const Mode mode_;
    bool signalled_;
    std::deque<WaiterId> waiters_;
};

/// A recursive mutex with an owner.
class KMutex final : public KObject {
public:
    static constexpr ObjectKind kKind = ObjectKind::Mutex;

    KMutex();

    /// Take the lock. True when `owner` holds it now and must not park; false
    /// when it was registered as a waiter. Re-entrant: an owner that already
    /// holds it just deepens the count.
    bool PrepareLock(WaiterId owner);

    struct ReleaseResult {
        /// False when `owner` was not the owner -- nothing changed.
        bool released = false;
        /// At most one waiter, whoever was queued first. The lock is *free* when
        /// this is returned: the woken waiter re-runs PrepareLock.
        std::vector<WaiterId> woken;
    };
    ReleaseResult Release(WaiterId owner);

    bool held() const;
    std::uint32_t recursion() const;
    bool owned_by(WaiterId owner) const;
    std::size_t waiter_count() const;

private:
    mutable std::mutex mutex_;
    WaiterId owner_ = 0;
    std::uint32_t recursion_ = 0;
    std::deque<WaiterId> waiters_;
};

/// A counting semaphore.
class KSemaphore final : public KObject {
public:
    static constexpr ObjectKind kKind = ObjectKind::Semaphore;

    KSemaphore(std::int32_t initial, std::int32_t maximum);

    /// Take a count. True when `waiter` must park; false when it took one.
    bool PrepareWait(WaiterId waiter);
    bool CancelWait(WaiterId waiter);

    struct SignalResult {
        /// False when the signal would push the count past the maximum. Nothing
        /// changed in that case -- the caller gets an error, not a clamp.
        bool ok = false;
        std::vector<WaiterId> woken;
    };
    SignalResult Signal(std::int32_t count);

    std::int32_t count() const;
    std::int32_t maximum() const noexcept;
    std::size_t waiter_count() const;

private:
    mutable std::mutex mutex_;
    std::int32_t count_;
    const std::int32_t maximum_;
    std::deque<WaiterId> waiters_;
};

}  // namespace zlong::service
