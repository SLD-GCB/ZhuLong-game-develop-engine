// 烛龙 (ZhuLong) - audout: the guest's path to the audio device.
//
// The console's real service is `audout:u`: open an output, start it, append buffers of
// PCM, and be told by an event when one has played. The command ids and buffer structs
// below are THIS PROJECT'S PLACEHOLDER, like every other layout in this layer.
//
// What is not a placeholder is where the samples end up. They go to an `audio::Sink` --
// the same seam the engine's own mixer writes to -- so a game's audio and the engine's
// take the same road out, and a test can count what a guest sent without a sound card.
//
// Two things are deliberately narrower than the console, and say so rather than
// pretending:
//
//   * the device is 48 kHz stereo, which is what this layer's mixer works at. An output
//     asking for another rate or channel count is refused with a reason, not quietly
//     resampled behind the guest's back.
//   * a buffer is drained into the sink on the call that appends it, so it is released
//     immediately and the event is signalled straight away. There is no device thread
//     yet; a real one would hold the buffer until it had actually played.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "zlong/audio/format.h"
#include "zlong/audio/sink.h"
#include "zlong/service/handle.h"
#include "zlong/service/registry.h"

namespace zlong::service {

class AudOutService final : public IService {
public:
    /// Commands, in the guest's command buffer. Placeholder ids.
    enum Command : std::uint32_t {
        /// data: u32 sample rate, u32 channel count.
        /// response: u32 output id, u32 sample rate, u32 channel count, u32 event handle.
        kCommandOpen = 1,
        /// data: u32 output id, u32 guest address, u32 byte size, u32 tag.
        /// The buffer holds the PCM. response: none.
        kCommandAppend = 2,
        /// data: u32 output id. response: u32 address, u32 byte size, u32 tag, u32 left.
        ///
        /// Takes back one buffer that has been played, in the order they arrived. A tag
        /// of zero means there was nothing to take.
        kCommandReleased = 3,
        /// data: u32 output id. response: none.
        kCommandStart = 4,
        /// data: u32 output id. response: none.
        kCommandStop = 5,
        /// data: u32 output id. response: u32 state: 0 stopped, 1 started.
        kCommandState = 6,
    };

    /// Failure results. Nonzero, in the same placeholder family as the other services.
    enum : std::uint32_t {
        kResultBadArgument = 0x5A4F'0001u,
        kResultNoOutput = 0x5A4F'0002u,
        kResultNoSink = 0x5A4F'0003u,
        kResultNotStarted = 0x5A4F'0004u,
        kResultBufferUnreadable = 0x5A4F'0005u,
        kResultUnsupportedFormat = 0x5A4F'0006u,
    };

    enum class State : std::uint32_t { Stopped = 0, Started = 1 };

    const char* name() const noexcept override { return "audout"; }

    /// Wire the device in. Borrowed, and may be null: with nothing behind it an append
    /// is refused with a reason rather than dropping the guest's audio on the floor.
    void set_sink(audio::Sink* sink) noexcept { sink_ = sink; }
    audio::Sink* sink() const noexcept { return sink_; }

    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;

    // --- what it has served, for tests and diagnosis ------------------------
    std::size_t output_count() const noexcept { return outputs_.size(); }
    /// Frames handed to the sink since the last open.
    std::uint64_t frames_written() const noexcept { return frames_written_; }
    std::uint32_t unknown_commands() const noexcept { return unknown_commands_; }
    /// The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }

private:
    /// One open output: the console's IAudioOut, minus the parts not modelled.
    struct Output {
        State state = State::Stopped;
        /// The guest's handle for the release event, so an append can signal it.
        Handle event = kInvalidHandle;
        /// What has been played and not yet taken back.
        struct Buffer {
            std::uint32_t address = 0;
            std::uint32_t size = 0;
            std::uint32_t tag = 0;
        };
        std::vector<Buffer> released;
    };

    void Open(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void Append(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void TakeReleased(const IpcRequest& request, IpcResponse& response);
    void SetState(const IpcRequest& request, IpcResponse& response, State state);
    void ReportState(const IpcRequest& request, IpcResponse& response);

    Output* Find(std::uint32_t id);
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);

    audio::Sink* sink_ = nullptr;

    std::map<std::uint32_t, Output> outputs_;
    std::uint32_t next_output_ = 1;
    std::uint64_t frames_written_ = 0;
    std::uint32_t unknown_commands_ = 0;
    std::string last_error_;
};

}  // namespace zlong::service
