#include "zlong/service/services/debug_service.h"

namespace zlong::service {

void DebugService::HandleRequest(ServiceContext& context, const IpcRequest& request,
                                 IpcResponse& response) {
    (void)context;  // this service needs nothing but what it counts

    switch (request.command_id) {
    case kCommandPing:
        ++ping_count;
        response.result = 0;
        response.data = {
            static_cast<std::uint8_t>(kPingAnswer & 0xFFu),
            static_cast<std::uint8_t>((kPingAnswer >> 8) & 0xFFu),
            static_cast<std::uint8_t>((kPingAnswer >> 16) & 0xFFu),
            static_cast<std::uint8_t>((kPingAnswer >> 24) & 0xFFu),
        };
        return;

    case kCommandEcho:
        ++echo_count;
        response.result = 0;
        response.data = request.data;
        return;

    default:
        // An unknown command is a failure with a nonzero result, not an empty
        // success: the guest asked for something and did not get it.
        ++unknown_count;
        response.result = kIpcResultUnknownCommand;
        return;
    }
}

}  // namespace zlong::service
