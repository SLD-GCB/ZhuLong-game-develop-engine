#include "zlong/service/services/audout_service.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../word_io.h"
#include "zlong/service/ipc.h"
#include "zlong/service/kernel.h"
#include "zlong/service/object.h"
#include "zlong/service/process.h"
#include "zlong/service/sync.h"

namespace zlong::service {

namespace {

/// Read one word a guest buffer names, or fail. The address is a *guest* address, which
/// is why this goes through the memory the request arrived with rather than a pointer.
bool ReadGuest32(cpu::GuestMemory& memory, std::uint64_t address, std::uint32_t& out) {
    const auto packed = memory.TryRead32(address);
    if (!packed.has_value()) {
        return false;
    }
    out = *packed;
    return true;
}

}  // namespace

bool AudOutService::Refuse(IpcResponse& response, std::uint32_t result, const std::string& why) {
    last_error_ = why;
    response.result = result;
    return false;
}

AudOutService::Output* AudOutService::Find(std::uint32_t id) {
    const auto found = outputs_.find(id);
    return found == outputs_.end() ? nullptr : &found->second;
}

void AudOutService::HandleRequest(ServiceContext& context, const IpcRequest& request,
                                  IpcResponse& response) {
    switch (request.command_id) {
    case kCommandOpen:
        Open(context, request, response);
        break;
    case kCommandAppend:
        Append(context, request, response);
        break;
    case kCommandReleased:
        TakeReleased(request, response);
        break;
    case kCommandStart:
        SetState(request, response, State::Started);
        break;
    case kCommandStop:
        SetState(request, response, State::Stopped);
        break;
    case kCommandState:
        ReportState(request, response);
        break;
    default:
        ++unknown_commands_;
        Refuse(response, kIpcResultUnknownCommand, "audout does not know that command");
        break;
    }
}

void AudOutService::Open(ServiceContext& context, const IpcRequest& request,
                         IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    if (!words.U32(rate) || !words.U32(channels)) {
        Refuse(response, kResultBadArgument, "OpenAudioOut needs a sample rate and a channel count");
        return;
    }
    if (rate != audio::kSampleRate || channels != audio::kChannels) {
        Refuse(response, kResultUnsupportedFormat,
               "the device is " + std::to_string(audio::kSampleRate) + " Hz and " +
                   std::to_string(audio::kChannels) + " channel(s)");
        return;
    }

    // The release event belongs to the guest, so it has to be published in the guest's
    // handle table. An auto event, because the console signals it once per buffer.
    KProcess* process = context.kernel.process();
    if (process == nullptr) {
        Refuse(response, kResultNoOutput, "no process is bound, so no event can be published");
        return;
    }
    auto* event = new KEvent(KEvent::Mode::Auto, /*initially_signalled=*/false);
    const Handle handle = process->handles().Allocate(event);
    event->Release();  // the table holds the reference now, whichever way this went
    if (handle == kInvalidHandle) {
        Refuse(response, kResultNoOutput, "the handle table is full");
        return;
    }

    Output output;
    output.event = handle;
    const std::uint32_t id = next_output_++;
    outputs_.emplace(id, output);

    word_io::PutU32(response.data, id);
    word_io::PutU32(response.data, rate);
    word_io::PutU32(response.data, channels);
    word_io::PutU32(response.data, handle);
    response.result = 0;
    last_error_.clear();
}

void AudOutService::Append(ServiceContext& context, const IpcRequest& request,
                           IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t tag = 0;
    if (!words.U32(id) || !words.U32(address) || !words.U32(size) || !words.U32(tag)) {
        Refuse(response, kResultBadArgument,
               "AppendAudioOutBuffer needs an output, an address, a size and a tag");
        return;
    }

    Output* output = Find(id);
    if (output == nullptr) {
        Refuse(response, kResultNoOutput, "that output id is not open");
        return;
    }
    if (output->state != State::Started) {
        Refuse(response, kResultNotStarted, "the output has not been started");
        return;
    }
    if (sink_ == nullptr) {
        Refuse(response, kResultNoSink, "no audio device is attached");
        return;
    }

    // Whole frames only. A trailing half-frame is not audio, and silently dropping it
    // would be this layer deciding what part of the guest's buffer to play.
    const std::uint32_t frame_bytes =
        static_cast<std::uint32_t>(sizeof(std::int16_t)) * audio::kChannels;
    if (size == 0 || size % frame_bytes != 0) {
        Refuse(response, kResultBadArgument,
               "the buffer is " + std::to_string(size) + " bytes, which is not whole " +
                   std::to_string(frame_bytes) + "-byte frames");
        return;
    }
    const std::uint32_t whole = size;

    // Interleaved sixteen-bit stereo: one frame per four bytes, low half first because
    // the guest is little-endian too.
    std::vector<audio::Frame> frames;
    frames.reserve(whole / frame_bytes);
    for (std::uint32_t at = 0; at < whole; at += frame_bytes) {
        std::uint32_t packed = 0;
        if (!ReadGuest32(context.core.Memory(), static_cast<std::uint64_t>(address) + at, packed)) {
            Refuse(response, kResultBufferUnreadable,
                   "the buffer is not readable at the address it names");
            return;
        }
        const auto left = static_cast<std::int16_t>(packed & 0xFFFFu);
        const auto right = static_cast<std::int16_t>((packed >> 16) & 0xFFFFu);
        frames.push_back(audio::Frame{audio::FromPcm16(left), audio::FromPcm16(right)});
    }

    if (!sink_->Submit(std::span<const audio::Frame>(frames))) {
        Refuse(response, kResultNoSink, "the device would not take the frames");
        return;
    }
    frames_written_ += frames.size();

    // Taken and played in the same breath, so it is released here and the event says so.
    output->released.push_back(Output::Buffer{address, size, tag});
    if (output->event != kInvalidHandle) {
        context.kernel.SignalEventHandle(output->event);
    }
    response.result = 0;
    last_error_.clear();
}

void AudOutService::TakeReleased(const IpcRequest& request, IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    if (!words.U32(id)) {
        Refuse(response, kResultBadArgument, "GetReleasedAudioOutBuffer needs an output");
        return;
    }
    Output* output = Find(id);
    if (output == nullptr) {
        Refuse(response, kResultNoOutput, "that output id is not open");
        return;
    }

    // Nothing played since the last call: a zero tag, rather than a buffer that was
    // never sent.
    if (output->released.empty()) {
        word_io::PutU32(response.data, 0);
        word_io::PutU32(response.data, 0);
        word_io::PutU32(response.data, 0);
        word_io::PutU32(response.data, 0);
        response.result = 0;
        last_error_.clear();
        return;
    }

    const Output::Buffer buffer = output->released.front();
    output->released.erase(output->released.begin());
    word_io::PutU32(response.data, buffer.address);
    word_io::PutU32(response.data, buffer.size);
    word_io::PutU32(response.data, buffer.tag);
    word_io::PutU32(response.data, static_cast<std::uint32_t>(output->released.size()));
    response.result = 0;
    last_error_.clear();
}

void AudOutService::SetState(const IpcRequest& request, IpcResponse& response, State state) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    if (!words.U32(id)) {
        Refuse(response, kResultBadArgument, "StartAudioOut and StopAudioOut need an output");
        return;
    }
    Output* output = Find(id);
    if (output == nullptr) {
        Refuse(response, kResultNoOutput, "that output id is not open");
        return;
    }
    output->state = state;
    response.result = 0;
    last_error_.clear();
}

void AudOutService::ReportState(const IpcRequest& request, IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    if (!words.U32(id)) {
        Refuse(response, kResultBadArgument, "GetAudioOutState needs an output");
        return;
    }
    Output* output = Find(id);
    if (output == nullptr) {
        Refuse(response, kResultNoOutput, "that output id is not open");
        return;
    }
    word_io::PutU32(response.data, static_cast<std::uint32_t>(output->state));
    response.result = 0;
    last_error_.clear();
}

}  // namespace zlong::service
