// 烛龙 (ZhuLong) - time.
//
// What this can honestly answer is an interval: the CPU's counter, and the
// frequency it ticks at. That is what a guest measuring elapsed time needs, and
// it is real rather than invented.
//
// What it CANNOT answer is a time of day. A wall clock needs an epoch this
// layer does not have -- nothing has told it what the date is -- and making one
// up would put a wrong date in the guest's hands with nothing to say so. So the
// date commands are simply not here, and a guest that asks for one gets the
// nonzero "unknown command" result instead of a plausible-looking number.
//
// PLACEHOLDER, like every other command layout in this layer: the command ids
// below are ours, not the console's.

#pragma once

#include <cstdint>

#include "zlong/service/registry.h"

namespace zlong::service {

class TimeService final : public IService {
public:
    enum Command : std::uint32_t {
        /// data: none. response: u64 nanoseconds since the counter started.
        kCommandGetUptime = 1,
        /// data: none. response: u64 counter ticks, u32 ticks per second.
        kCommandGetCounter = 2,
    };

    const char* name() const noexcept override { return "time"; }

    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;

    std::uint32_t queries() const noexcept { return queries_; }
    std::uint32_t unknown_commands() const noexcept { return unknown_commands_; }

private:
    std::uint32_t queries_ = 0;
    std::uint32_t unknown_commands_ = 0;
};

}  // namespace zlong::service
