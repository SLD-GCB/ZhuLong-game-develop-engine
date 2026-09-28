#include "zlong/service/sync.h"

#include "waiter_queue.h"

namespace zlong::service {

// ---------------------------------------------------------------- KEvent ----

KEvent::KEvent(Mode mode, bool initially_signalled)
    : KObject(kKind), mode_(mode), signalled_(initially_signalled) {}

bool KEvent::PrepareWait(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (signalled_) {
        if (mode_ == Mode::Auto) {
            signalled_ = false;  // an auto event's signal is consumed by one wait
        }
        return false;
    }
    EnqueueWaiter(waiters_, waiter);
    return true;
}

bool KEvent::CancelWait(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DequeueWaiter(waiters_, waiter);
}

std::vector<WaiterId> KEvent::Signal() {
    std::vector<WaiterId> woken;
    std::lock_guard<std::mutex> lock(mutex_);

    if (mode_ == Mode::Manual) {
        signalled_ = true;
        woken.assign(waiters_.begin(), waiters_.end());
        waiters_.clear();
        return woken;
    }

    // Auto: the signal is LEVEL-triggered. It stays set and whichever waiter
    // retries first consumes it, in PrepareWait.
    //
    // Consuming it here, on one named waiter's behalf, is the tempting version
    // and it loses the signal: the named waiter has to retry, and its retry sees
    // "not signalled" and parks again -- with nobody left to signal it.
    signalled_ = true;
    if (!waiters_.empty()) {
        woken.push_back(waiters_.front());
        waiters_.pop_front();
    }
    return woken;
}

void KEvent::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    signalled_ = false;
}

bool KEvent::signalled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signalled_;
}

KEvent::Mode KEvent::mode() const noexcept { return mode_; }

std::size_t KEvent::waiter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

// ---------------------------------------------------------------- KMutex ----

KMutex::KMutex() : KObject(kKind) {}

bool KMutex::PrepareLock(WaiterId owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recursion_ == 0) {
        owner_ = owner;
        recursion_ = 1;
        return true;
    }
    if (owner_ == owner) {
        ++recursion_;
        return true;
    }
    EnqueueWaiter(waiters_, owner);
    return false;
}

KMutex::ReleaseResult KMutex::Release(WaiterId owner) {
    ReleaseResult result;
    std::lock_guard<std::mutex> lock(mutex_);

    if (recursion_ == 0 || owner_ != owner) {
        return result;  // not the owner: nothing changes
    }
    result.released = true;

    if (--recursion_ > 0) {
        return result;  // still held, one level shallower
    }

    owner_ = 0;
    // Hand the lock back free and wake the first waiter, which re-runs
    // PrepareLock. Ownership is deliberately not transferred: a wait that gets
    // cancelled in between must not leave anyone holding the lock unknowingly.
    if (!waiters_.empty()) {
        result.woken.push_back(waiters_.front());
        waiters_.pop_front();
    }
    return result;
}

bool KMutex::held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recursion_ != 0;
}

std::uint32_t KMutex::recursion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recursion_;
}

bool KMutex::owned_by(WaiterId owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recursion_ != 0 && owner_ == owner;
}

std::size_t KMutex::waiter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

// ------------------------------------------------------------ KSemaphore ----

KSemaphore::KSemaphore(std::int32_t initial, std::int32_t maximum)
    : KObject(kKind), count_(initial), maximum_(maximum) {}

bool KSemaphore::PrepareWait(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ > 0) {
        --count_;
        return false;
    }
    EnqueueWaiter(waiters_, waiter);
    return true;
}

bool KSemaphore::CancelWait(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DequeueWaiter(waiters_, waiter);
}

KSemaphore::SignalResult KSemaphore::Signal(std::int32_t count) {
    SignalResult result;
    std::lock_guard<std::mutex> lock(mutex_);

    if (count <= 0 || count_ + count > maximum_) {
        return result;  // refused, not clamped: the caller gets an error
    }

    // LEVEL-triggered for the same reason as the event: the count is what the
    // waiters race for, so it is added here and each waiter takes its own on
    // retry. Taking one on a named waiter's behalf would lose it whenever that
    // waiter failed to come back.
    count_ += count;
    std::int32_t remaining = count;
    while (remaining > 0 && !waiters_.empty()) {
        result.woken.push_back(waiters_.front());
        waiters_.pop_front();
        --remaining;
    }
    result.ok = true;
    return result;
}

std::int32_t KSemaphore::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

std::int32_t KSemaphore::maximum() const noexcept { return maximum_; }

std::size_t KSemaphore::waiter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

}  // namespace zlong::service
