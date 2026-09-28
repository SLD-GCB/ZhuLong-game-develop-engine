// 烛龙 (ZhuLong) - a connection to a service.
//
// A session is what an IPC request travels on, and it serialises those requests.
// Two cores sending on one session must not run the handler at the same time: a
// service that is not written to be re-entrant would corrupt its own state, and
// nothing would say so. The serialisation lives here rather than in each service
// so it cannot be forgotten.
//
// A blocked sender parks and retries, exactly like every other wait in this
// layer: BeginRequest returns whether the caller must park, and a woken caller
// re-runs it from the top.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "zlong/service/object.h"
#include "zlong/service/sync.h"

namespace zlong::service {

/// Implemented by the service registry. Forward-declared so a session does not
/// have to pull the registry's interface in.
class IService;

class KSession final : public KObject {
public:
    static constexpr ObjectKind kKind = ObjectKind::Session;

    KSession(std::uint64_t session_id, std::uint64_t client_pid, std::string service_name);

    std::uint64_t session_id() const noexcept { return session_id_; }
    std::uint64_t client_pid() const noexcept { return client_pid_; }
    const std::string& service_name() const noexcept { return service_name_; }

    /// Attach the service this session talks to. Set once, at connect time,
    /// before the session is handed to the guest.
    void Attach(IService* service) noexcept;
    IService* service() const;

    /// True when `waiter` may send now; false when another request is in flight,
    /// in which case `waiter` was registered and must park and retry.
    bool BeginRequest(WaiterId waiter);

    /// Drop a registration that will not park.
    bool CancelRequestWait(WaiterId waiter);

    /// Finish a request. Returns at most one waiter to wake; it re-runs
    /// BeginRequest, so ownership of the session is never handed over silently.
    std::vector<WaiterId> EndRequest();

    bool request_in_flight() const;
    std::size_t waiter_count() const;

private:
    const std::uint64_t session_id_;
    const std::uint64_t client_pid_;
    const std::string service_name_;
    mutable std::mutex mutex_;
    IService* service_ = nullptr;
    bool in_flight_ = false;
    std::deque<WaiterId> waiters_;
};

}  // namespace zlong::service
