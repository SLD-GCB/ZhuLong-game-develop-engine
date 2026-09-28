// 烛龙 (ZhuLong) - audren: the guest's mixer.
//
// Where audout is handed a buffer of finished PCM and passes it to the device, audren
// is handed *voices* -- a sample address, a rate, a pitch, a gain -- and produces the
// mix itself. That is the whole difference, and it is why this service reaches for the
// audio layer's mixer rather than only a sink: `audio::Voice` and `audio::Mix` already
// mean "sum these clips into frames, converting rate on the way in", which is what the
// console's renderer does with far more knobs than are modelled here.
//
// The command ids and the voice descriptor below are THIS PROJECT'S PLACEHOLDER, like
// every other layout in this layer. The console's `audren:u` takes a config struct of
// several hundred bytes per voice, an effect graph, and a revision number.
//
// Three things are deliberately narrower than the console, and say so rather than
// pretending:
//
//   * the device is 48 kHz stereo, the rate this layer's mixer works at. A renderer
//     asking for another rate is refused with a reason, not quietly resampled.
//   * a voice's samples are read out of guest memory once, by the call that registers
//     it, and the service owns the copy from then on. The console re-reads the wave
//     buffer every update, which is what lets a guest stream into a ring buffer; here
//     a guest that streams has to register again to be heard, and registering restarts
//     that voice's cursor. Re-reading per update would let the samples change under a
//     cursor that had already advanced, which is harder to be right about than a
//     documented limit.
//   * there is no effect graph and no worker thread. An update mixes the frames the
//     guest asks for and returns; nothing renders ahead of a request.
//
// What is NOT narrowed is where the frames go. They land in the guest's own output
// buffer, because that is what makes this a renderer rather than a player -- and, when
// a device is attached, the same `audio::Sink` audout and the engine's mixer write to,
// so a game that uses the renderer is heard without a second path out.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "zlong/audio/format.h"
#include "zlong/audio/mixer.h"
#include "zlong/audio/sink.h"
#include "zlong/service/handle.h"
#include "zlong/service/registry.h"

namespace zlong::service {

class AudRenService final : public IService {
public:
    /// Commands, in the guest's command buffer. Placeholder ids.
    enum Command : std::uint32_t {
        /// data: u32 sample rate, u32 channel count.
        /// response: u32 renderer id, u32 frames per update at most, u32 event handle.
        kCommandOpen = 1,
        /// data: u32 renderer, u32 voice count, then `count` descriptors of seven u32:
        ///   sample address, sample byte size, sample rate, pitch, gain left, gain right,
        ///   loop.
        ///
        /// The samples are interleaved sixteen-bit stereo, the shape audout reads, at
        /// the rate the descriptor names. Pitch and both gains are Q16.16: 65536 is
        /// 1.0. response: u32 voices accepted.
        kCommandSetVoices = 2,
        /// data: u32 renderer, u32 guest output address, u32 frame count.
        /// response: u32 frames written, u32 voices still playing, u32 voices finished.
        ///
        /// Mixes `frame count` frames from the registered voices and writes them to the
        /// guest's buffer as interleaved sixteen-bit stereo, advancing every voice.
        kCommandUpdate = 3,
        /// data: u32 renderer. response: none. The renderer's voices are dropped and
        /// its event handle is retired.
        kCommandClose = 4,
    };

    /// Failure results. Nonzero, in the same placeholder family as the other services.
    enum : std::uint32_t {
        kResultBadArgument = 0x5A52'0001u,
        kResultNoRenderer = 0x5A52'0002u,
        kResultUnsupportedFormat = 0x5A52'0003u,
        kResultBufferUnreadable = 0x5A52'0004u,
        kResultOutputUnwritable = 0x5A52'0005u,
        kResultTooMuch = 0x5A52'0006u,
    };

    /// The most frames one update may ask for: a second at the device rate. A bound
    /// rather than a promise -- it keeps the scratch block a guest-sized number.
    static constexpr std::uint32_t kMaxFramesPerUpdate = audio::kSampleRate;
    /// The most voices a renderer holds.
    static constexpr std::uint32_t kMaxVoices = 64;
    /// The most bytes one voice's samples may take. Sixteen mebibytes is about eighty
    /// seconds of stereo at the device rate, and it is a bound for the same reason.
    static constexpr std::uint32_t kMaxClipBytes = 16u * 1024u * 1024u;
    /// One descriptor: seven words.
    static constexpr std::size_t kDescriptorWords = 7;

    const char* name() const noexcept override { return "audren"; }

    /// Wire the device in. Borrowed, and may be null: with nothing behind it an update
    /// still renders into the guest's buffer, it is just not played.
    void set_sink(audio::Sink* sink) noexcept { sink_ = sink; }
    audio::Sink* sink() const noexcept { return sink_; }

    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;

    // --- what it has served, for tests and diagnosis ------------------------
    std::size_t renderer_count() const noexcept { return renderers_.size(); }
    /// Frames mixed into a guest buffer since the last open.
    std::uint64_t frames_rendered() const noexcept { return frames_rendered_; }
    /// Updates whose frames reached the guest but that the device would not take. Not
    /// a failure: the guest already has what it asked for.
    std::uint32_t sink_refusals() const noexcept { return sink_refusals_; }
    std::uint32_t unknown_commands() const noexcept { return unknown_commands_; }
    /// The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }

private:
    /// One open renderer: what the console calls an audio renderer, minus the effects.
    struct Renderer {
        /// The guest's handle for "an update finished", so an update can signal it.
        Handle event = kInvalidHandle;
        /// The clips the voices read. A deque and not a vector, because `Voice::clip`
        /// is a pointer into it: a deque never moves an element it already holds, and
        /// a vector would move every one of them on the next push.
        std::deque<audio::Clip> clips;
        /// Parallel to `clips`, and holding a cursor that outlives each update.
        std::vector<audio::Voice> voices;
        /// Reused across updates so a render does not allocate.
        std::vector<audio::Frame> block;
    };

    void Open(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void SetVoices(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void Update(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void Close(ServiceContext& context, const IpcRequest& request, IpcResponse& response);

    Renderer* Find(std::uint32_t id);
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);

    audio::Sink* sink_ = nullptr;

    std::map<std::uint32_t, Renderer> renderers_;
    std::uint32_t next_renderer_ = 1;
    std::uint64_t frames_rendered_ = 0;
    std::uint32_t sink_refusals_ = 0;
    std::uint32_t unknown_commands_ = 0;
    std::string last_error_;
};

}  // namespace zlong::service
