#include "zlong/service/services/audren_service.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../word_io.h"
#include "zlong/service/ipc.h"
#include "zlong/service/kernel.h"
#include "zlong/service/object.h"
#include "zlong/service/process.h"
#include "zlong/service/sync.h"

namespace zlong::service {

namespace {

/// A Q16.16 guest number as the double the mixer works in. 65536 is 1.0.
double FromQ16(std::uint32_t value) {
    return static_cast<double>(value) / 65536.0;
}

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

/// How many voices have not run out.
std::uint32_t Playing(const std::vector<audio::Voice>& voices) {
    std::uint32_t playing = 0;
    for (const audio::Voice& voice : voices) {
        if (!voice.finished) {
            ++playing;
        }
    }
    return playing;
}

}  // namespace

bool AudRenService::Refuse(IpcResponse& response, std::uint32_t result, const std::string& why) {
    last_error_ = why;
    response.result = result;
    return false;
}

AudRenService::Renderer* AudRenService::Find(std::uint32_t id) {
    const auto found = renderers_.find(id);
    return found == renderers_.end() ? nullptr : &found->second;
}

void AudRenService::HandleRequest(ServiceContext& context, const IpcRequest& request,
                                  IpcResponse& response) {
    switch (request.command_id) {
    case kCommandOpen:
        Open(context, request, response);
        break;
    case kCommandSetVoices:
        SetVoices(context, request, response);
        break;
    case kCommandUpdate:
        Update(context, request, response);
        break;
    case kCommandClose:
        Close(context, request, response);
        break;
    default:
        ++unknown_commands_;
        Refuse(response, kIpcResultUnknownCommand, "audren does not know that command");
        break;
    }
}

void AudRenService::Open(ServiceContext& context, const IpcRequest& request,
                         IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t rate = 0;
    std::uint32_t channels = 0;
    if (!words.U32(rate) || !words.U32(channels)) {
        Refuse(response, kResultBadArgument,
               "OpenAudioRenderer needs a sample rate and a channel count");
        return;
    }
    if (rate != audio::kSampleRate || channels != audio::kChannels) {
        Refuse(response, kResultUnsupportedFormat,
               "the device is " + std::to_string(audio::kSampleRate) + " Hz and " +
                   std::to_string(audio::kChannels) + " channel(s)");
        return;
    }

    // The finish event belongs to the guest, so it has to be published in the guest's
    // handle table. An auto event, because the console signals one per update.
    KProcess* process = context.kernel.process();
    if (process == nullptr) {
        Refuse(response, kResultNoRenderer, "no process is bound, so no event can be published");
        return;
    }
    auto* event = new KEvent(KEvent::Mode::Auto, /*initially_signalled=*/false);
    const Handle handle = process->handles().Allocate(event);
    event->Release();  // the table holds the reference now, whichever way this went
    if (handle == kInvalidHandle) {
        Refuse(response, kResultNoRenderer, "the handle table is full");
        return;
    }

    Renderer renderer;
    renderer.event = handle;
    const std::uint32_t id = next_renderer_++;
    renderers_.emplace(id, std::move(renderer));

    word_io::PutU32(response.data, id);
    word_io::PutU32(response.data, kMaxFramesPerUpdate);
    word_io::PutU32(response.data, handle);
    response.result = 0;
    last_error_.clear();
}

void AudRenService::SetVoices(ServiceContext& context, const IpcRequest& request,
                              IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    std::uint32_t count = 0;
    if (!words.U32(id) || !words.U32(count)) {
        Refuse(response, kResultBadArgument, "SetAudioRendererVoices needs a renderer and a count");
        return;
    }
    Renderer* renderer = Find(id);
    if (renderer == nullptr) {
        Refuse(response, kResultNoRenderer, "that renderer id is not open");
        return;
    }
    if (count > kMaxVoices) {
        Refuse(response, kResultTooMuch,
               std::to_string(count) + " voices is more than the " + std::to_string(kMaxVoices) +
                   " this renderer holds");
        return;
    }
    // Every descriptor has to be present before anything is replaced. A request that stops
    // mid-list is a refusal that leaves the renderer playing what it had, rather than one
    // that registers the first few voices and silently drops the rest.
    if (words.remaining() < static_cast<std::size_t>(count) * kDescriptorWords * 4u) {
        Refuse(response, kResultBadArgument, "the request stops before the last voice descriptor");
        return;
    }

    // One frame of the guest's samples is one word: interleaved sixteen-bit stereo, low
    // half first, the shape audout reads.
    const std::uint32_t frame_bytes =
        static_cast<std::uint32_t>(sizeof(std::int16_t)) * audio::kChannels;

    // Parsed apart from the renderer until every voice is good, so a bad one in the
    // middle cannot leave the renderer holding half of a new list.
    std::vector<audio::Clip> parsed;
    std::vector<double> steps;
    std::vector<std::array<audio::Sample, audio::kChannels>> gains;
    std::vector<bool> loops;
    parsed.reserve(count);

    for (std::uint32_t voice = 0; voice < count; ++voice) {
        std::uint32_t address = 0;
        std::uint32_t size = 0;
        std::uint32_t rate = 0;
        std::uint32_t pitch = 0;
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        std::uint32_t loop = 0;
        if (!words.U32(address) || !words.U32(size) || !words.U32(rate) || !words.U32(pitch) ||
            !words.U32(left) || !words.U32(right) || !words.U32(loop)) {
            Refuse(response, kResultBadArgument, "the request stops before the last voice descriptor");
            return;
        }
        if (size == 0 || size % frame_bytes != 0) {
            Refuse(response, kResultBadArgument,
                   "voice " + std::to_string(voice) + " is " + std::to_string(size) +
                       " bytes, which is not whole " + std::to_string(frame_bytes) + "-byte frames");
            return;
        }
        if (size > kMaxClipBytes) {
            Refuse(response, kResultTooMuch,
                   "voice " + std::to_string(voice) + " asks for " + std::to_string(size) +
                       " bytes, more than the " + std::to_string(kMaxClipBytes) + " one clip may take");
            return;
        }
        if (rate == 0) {
            Refuse(response, kResultBadArgument,
                   "voice " + std::to_string(voice) + " has no sample rate to play at");
            return;
        }
        if (pitch == 0) {
            Refuse(response, kResultBadArgument,
                   "voice " + std::to_string(voice) + " has a pitch of zero, so it would never advance");
            return;
        }

        audio::Clip clip;
        clip.rate = rate;
        clip.frames.reserve(size / frame_bytes);
        for (std::uint32_t at = 0; at < size; at += frame_bytes) {
            std::uint32_t packed = 0;
            if (!ReadGuest32(context.core.Memory(), static_cast<std::uint64_t>(address) + at,
                             packed)) {
                Refuse(response, kResultBufferUnreadable,
                       "voice " + std::to_string(voice) +
                           "'s samples are not readable at the address they name");
                return;
            }
            const auto low = static_cast<std::int16_t>(packed & 0xFFFFu);
            const auto high = static_cast<std::int16_t>((packed >> 16) & 0xFFFFu);
            clip.frames.push_back(audio::Frame{audio::FromPcm16(low), audio::FromPcm16(high)});
        }

        parsed.push_back(std::move(clip));
        // The clip's own rate is what the mixer converts with; the guest's pitch rides on
        // top of it, so 65536 plays the clip at the pitch it was authored at.
        steps.push_back(audio::StepFor(parsed.back()) * FromQ16(pitch));
        gains.push_back({static_cast<audio::Sample>(FromQ16(left)),
                         static_cast<audio::Sample>(FromQ16(right))});
        loops.push_back(loop != 0);
    }

    // The clips move into the deque before any voice points at one: a deque never moves an
    // element it already holds, so every address taken below stays good for the renderer's
    // whole life. The two are swapped together because replacing the clips invalidates the
    // clips the old voices pointed at.
    std::deque<audio::Clip> clips;
    for (audio::Clip& clip : parsed) {
        clips.push_back(std::move(clip));
    }
    std::vector<audio::Voice> voices;
    voices.reserve(clips.size());
    for (std::size_t index = 0; index < clips.size(); ++index) {
        audio::Voice voice;
        voice.clip = &clips[index];
        voice.cursor = 0.0;
        voice.step = steps[index];
        voice.gain = gains[index];
        voice.loop = loops[index];
        voice.finished = false;
        voices.push_back(voice);
    }

    renderer->clips = std::move(clips);
    renderer->voices = std::move(voices);

    word_io::PutU32(response.data, static_cast<std::uint32_t>(renderer->voices.size()));
    response.result = 0;
    last_error_.clear();
}

void AudRenService::Update(ServiceContext& context, const IpcRequest& request,
                           IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    std::uint32_t address = 0;
    std::uint32_t frames = 0;
    if (!words.U32(id) || !words.U32(address) || !words.U32(frames)) {
        Refuse(response, kResultBadArgument,
               "RequestUpdateAudioRenderer needs a renderer, an address and a frame count");
        return;
    }
    Renderer* renderer = Find(id);
    if (renderer == nullptr) {
        Refuse(response, kResultNoRenderer, "that renderer id is not open");
        return;
    }
    if (frames == 0) {
        Refuse(response, kResultBadArgument, "an update asking for no frames");
        return;
    }
    if (frames > kMaxFramesPerUpdate) {
        Refuse(response, kResultTooMuch,
               std::to_string(frames) + " frames is more than the " +
                   std::to_string(kMaxFramesPerUpdate) + " one update may ask for");
        return;
    }

    // The mix advances every cursor as it goes, so it runs on a copy. If the guest's
    // output buffer turns out not to be writable, the renderer is then left exactly where
    // it was rather than a frame ahead of a buffer that was never delivered.
    std::vector<audio::Voice> trial = renderer->voices;
    std::vector<audio::Frame>& block = renderer->block;
    block.assign(frames, audio::Frame{});
    audio::Mix(std::span<audio::Voice>(trial), std::span<audio::Frame>(block));

    for (std::uint32_t index = 0; index < frames; ++index) {
        const audio::Frame& frame = block[index];
        const auto low = static_cast<std::uint32_t>(static_cast<std::uint16_t>(audio::ToPcm16(frame[0])));
        const auto high = static_cast<std::uint32_t>(static_cast<std::uint16_t>(audio::ToPcm16(frame[1])));
        const std::uint32_t packed = low | (high << 16);
        if (!context.core.Memory().TryWrite32(static_cast<std::uint64_t>(address) + index * 4u,
                                              packed)) {
            Refuse(response, kResultOutputUnwritable,
                   "the output buffer is not writable at the address it names");
            return;
        }
    }

    // The frames are in the guest's buffer now, so the mix really happened: the cursors
    // move even if the device below turns out not to want what was rendered.
    renderer->voices = std::move(trial);
    frames_rendered_ += frames;

    last_error_.clear();
    if (sink_ != nullptr && !sink_->Submit(std::span<const audio::Frame>(block))) {
        // Counted rather than fatal. The guest asked for frames in its own buffer and has
        // them; refusing the whole call would say nothing was rendered when something was.
        ++sink_refusals_;
        last_error_ = "the device would not take the rendered frames";
    }
    if (renderer->event != kInvalidHandle) {
        context.kernel.SignalEventHandle(renderer->event);
    }

    const std::uint32_t playing = Playing(renderer->voices);
    word_io::PutU32(response.data, frames);
    word_io::PutU32(response.data, playing);
    word_io::PutU32(response.data, static_cast<std::uint32_t>(renderer->voices.size()) - playing);
    response.result = 0;
}

void AudRenService::Close(ServiceContext& context, const IpcRequest& request,
                          IpcResponse& response) {
    word_io::Words words(request.data);
    std::uint32_t id = 0;
    if (!words.U32(id)) {
        Refuse(response, kResultBadArgument, "CloseAudioRenderer needs a renderer");
        return;
    }
    const auto found = renderers_.find(id);
    if (found == renderers_.end()) {
        Refuse(response, kResultNoRenderer, "that renderer id is not open");
        return;
    }

    // The event was published in the guest's table, so closing the renderer has to drop the
    // table's reference: leaving it would hand the guest a handle that outlives the thing
    // it was for. A process that dies closes all of them anyway; this is the renderer a
    // guest closes on purpose.
    KProcess* process = context.kernel.process();
    if (process != nullptr && found->second.event != kInvalidHandle) {
        process->handles().Close(found->second.event);
    }
    renderers_.erase(found);
    response.result = 0;
    last_error_.clear();
}

}  // namespace zlong::service
