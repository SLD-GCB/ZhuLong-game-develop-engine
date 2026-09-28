// 烛龙 (ZhuLong) - the first service.
//
// Deliberately trivial: it exists so the IPC path has somewhere to arrive, and so
// a break anywhere along that path shows up here as a missing call instead of as
// silence.
//
// It counts what it served, which is what lets a test tell "the service did the
// work" apart from "something happened to fill the buffer in".

#pragma once

#include "zlong/service/registry.h"

namespace zlong::service {

class DebugService final : public IService {
public:
    static constexpr std::uint32_t kCommandPing = 1;
    static constexpr std::uint32_t kCommandEcho = 2;

    /// This project's own answer to Ping, chosen so it cannot be confused with a
    /// value that happened to be lying in the buffer.
    static constexpr std::uint32_t kPingAnswer = 0x5A1D'0001u;

    const char* name() const noexcept override { return "debug"; }

    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;

    /// Served counts. Plain members, not atomics: the session serialises entry,
    /// so only one sender is ever inside HandleRequest.
    std::uint32_t ping_count = 0;
    std::uint32_t echo_count = 0;
    std::uint32_t unknown_count = 0;
};

}  // namespace zlong::service
