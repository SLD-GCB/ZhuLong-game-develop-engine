#include "zlong/service/services/time_service.h"

#include "../word_io.h"
#include "zlong/cpu/cpu.h"
#include "zlong/service/ipc.h"

namespace zlong::service {

using word_io::PutU32;
using word_io::PutU64;

namespace {

/// The counter counts ticks; a guest wants a duration. Integer maths on purpose:
/// a double would lose the low bits of a counter that runs for years.
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ull;

std::uint64_t TicksToNanoseconds(std::uint64_t ticks, std::uint32_t frequency) noexcept {
    if (frequency == 0) {
        return 0;
    }
    return (ticks / frequency) * kNanosecondsPerSecond +
           ((ticks % frequency) * kNanosecondsPerSecond) / frequency;
}

}  // namespace

void TimeService::HandleRequest(ServiceContext& context, const IpcRequest& request,
                                IpcResponse& response) {
    const cpu::Cpu& cpu = context.core.GetCpu();
    const std::uint64_t ticks = cpu.GetCNTPCT();
    const std::uint32_t frequency = cpu.CntfrqEl0();

    switch (request.command_id) {
    case kCommandGetUptime:
        ++queries_;
        response.result = 0;
        PutU64(response.data, TicksToNanoseconds(ticks, frequency));
        return;

    case kCommandGetCounter:
        ++queries_;
        response.result = 0;
        PutU64(response.data, ticks);
        PutU32(response.data, frequency);
        return;

    default:
        // A date request lands here, and that is the point: no epoch, so no
        // answer, rather than a made-up one.
        ++unknown_commands_;
        response.result = kIpcResultUnknownCommand;
        return;
    }
}

}  // namespace zlong::service
