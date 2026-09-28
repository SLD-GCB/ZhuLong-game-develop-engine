// 烛龙 (ZhuLong) - the waiter queue, shared by everything that has one.
//
// Internal to this layer: both the waitable objects and a session queue waiters
// the same way, and the duplicate check is not optional -- a wait is a retry
// loop, so the same waiter can come back before its previous registration has
// been consumed, and registering twice would leave a ghost in the queue that no
// later signal can ever clear.

#pragma once

#include <deque>

#include "zlong/service/sync.h"

namespace zlong::service {

/// Register a waiter unless it is already queued. False when it already was.
inline bool EnqueueWaiter(std::deque<WaiterId>& waiters, WaiterId waiter) {
    for (const WaiterId queued : waiters) {
        if (queued == waiter) {
            return false;
        }
    }
    waiters.push_back(waiter);
    return true;
}

/// Remove a waiter. False when it was not queued.
inline bool DequeueWaiter(std::deque<WaiterId>& waiters, WaiterId waiter) {
    for (auto it = waiters.begin(); it != waiters.end(); ++it) {
        if (*it == waiter) {
            waiters.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace zlong::service
