// 烛龙 (ZhuLong) - the service registry.
//
// A name maps to a service. Connecting creates a session; a session serialises
// its requests, so a service is never entered on two cores at once. That is why
// HandleRequest may touch plain members without a lock of its own.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zlong/service/ipc.h"
#include "zlong/service/session.h"

namespace zlong::cpu {
class Core;
}

namespace zlong::service {

class Kernel;

/// A service name is read out of guest memory, so its length is bounded here
/// rather than trusted.
inline constexpr std::size_t kMaxServiceNameBytes = 64;

/// What a service is told about the caller.
///
/// A service is a free-standing object with no chain of `this` back to the
/// kernel, and it has no state of its own to fall back on: resolving a path
/// needs the caller's mounts, reading the clock needs the caller's core. So
/// whatever it needs is handed over per request -- the same reasoning as
/// SvcContext, one layer down.
///
/// The session is here for identity. `kernel.process()` is the process that
/// request resolves against, which is the caller today and will need the
/// session to tell apart once there is more than one.
struct ServiceContext {
    cpu::Core& core;
    Kernel& kernel;
    KSession& session;
};

class IService {
public:
    virtual ~IService() = default;

    virtual const char* name() const noexcept = 0;

    /// Serve one request. The session is already serialised against every other
    /// sender, so only one core is ever inside this.
    virtual void HandleRequest(ServiceContext& context, const IpcRequest& request,
                               IpcResponse& response) = 0;
};

class ServiceRegistry {
public:
    ServiceRegistry() = default;

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    /// Register under `name`. Refuses an empty name or a duplicate: letting one
    /// silently shadow another would make a connect land somewhere unexpected.
    bool Register(std::string name, std::unique_ptr<IService> service);

    /// The service registered under `name`, or nullptr.
    IService* Find(std::string_view name) const;

    /// Registered names, in registration order.
    std::vector<std::string> names() const;

    std::size_t size() const noexcept { return services_.size(); }

private:
    struct Entry {
        std::string name;
        std::unique_ptr<IService> service;
    };
    std::vector<Entry> services_;
};

}  // namespace zlong::service
