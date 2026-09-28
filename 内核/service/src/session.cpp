#include "zlong/service/session.h"

#include <utility>

#include "waiter_queue.h"

namespace zlong::service {

KSession::KSession(std::uint64_t session_id, std::uint64_t client_pid, std::string service_name)
    : KObject(kKind),
      session_id_(session_id),
      client_pid_(client_pid),
      service_name_(std::move(service_name)) {}

void KSession::Attach(IService* service) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    service_ = service;
}

IService* KSession::service() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return service_;
}

bool KSession::BeginRequest(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!in_flight_) {
        in_flight_ = true;
        return true;
    }
    EnqueueWaiter(waiters_, waiter);
    return false;
}

bool KSession::CancelRequestWait(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DequeueWaiter(waiters_, waiter);
}

std::vector<WaiterId> KSession::EndRequest() {
    std::vector<WaiterId> woken;
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_ = false;
    // Leave the session free and wake the first waiter, which re-runs
    // BeginRequest. Ownership is not handed over, so a cancelled wait cannot
    // leave a session marked busy by somebody who is no longer waiting.
    if (!waiters_.empty()) {
        woken.push_back(waiters_.front());
        waiters_.pop_front();
    }
    return woken;
}

bool KSession::request_in_flight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_;
}

std::size_t KSession::waiter_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiters_.size();
}

}  // namespace zlong::service
