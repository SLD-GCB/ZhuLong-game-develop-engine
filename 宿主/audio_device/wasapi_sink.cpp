#include "wasapi_sink.h"

// Windows headers first and throttled: `windows.h` drags in macros (min, max) that
// collide with the standard library's, and the AUDCLNT_* names are only spelled out if
// the lean definitions are in place before it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace zlong::host {

namespace {

using audio::Frame;

/// One queue's worth: a fifth of a second. Large enough that a mixer submitting in
/// blocks does not stall on it, small enough that stopping means stopping soon.
constexpr std::size_t kQueueFrames = zlong::audio::kSampleRate / 5;

/// How long `Finish` waits for the device to drain before giving up on it. Generous:
/// the alternative is exiting while the last of the audio is still in the buffer.
constexpr int kDrainTimeoutMs = 5000;

/// A COM pointer that releases on the way out, so an early return cannot leak one.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** Put() {
        Reset();
        return &pointer_;
    }
    T* Get() const noexcept { return pointer_; }
    T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    void Reset() {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

std::string HResultText(HRESULT result) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(result));
    return text;
}

}  // namespace

struct WasapiSink::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;
    std::uint32_t buffer_frames = 0;
    /// How one frame lands in the device's buffer. Shared mode hands out float or
    /// sixteen-bit integer, and nothing else is taken.
    bool float32 = true;
    std::uint32_t frame_bytes = 0;

    std::thread worker;
    std::mutex mutex;
    std::condition_variable space;    // the queue has room
    std::condition_variable drained;  // everything submitted has played
    std::deque<Frame> queue;

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> played{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> silence{0};
    std::atomic<std::uint64_t> underruns{0};
    std::string failure;

    /// Fails the sink and wakes everyone waiting on it, so a device that goes away
    /// does not leave a caller blocked forever.
    void Fail(const std::string& why) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failure.empty()) {
                failure = why;
            }
        }
        failed = true;
        space.notify_all();
        drained.notify_all();
    }

    void Put(BYTE* at, const Frame& frame) const {
        if (float32) {
            auto* out = reinterpret_cast<float*>(at);
            out[0] = frame[0];
            out[1] = frame[1];
        } else {
            auto* out = reinterpret_cast<std::int16_t*>(at);
            out[0] = audio::ToPcm16(frame[0]);
            out[1] = audio::ToPcm16(frame[1]);
        }
    }

    /// Fill whatever room the device has, from the queue or from silence.
    void Pump() {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            Fail("the device would not report its buffer");
            return;
        }
        const UINT32 wanted = buffer_frames > padding ? buffer_frames - padding : 0;
        if (wanted == 0) {
            return;
        }

        BYTE* data = nullptr;
        const HRESULT got = render->GetBuffer(wanted, &data);
        if (FAILED(got)) {
            Fail("the device would not hand out a buffer (" + HResultText(got) + ")");
            return;
        }

        UINT32 written = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (; written < wanted; ++written) {
                if (queue.empty()) {
                    break;
                }
                Put(data + static_cast<std::size_t>(written) * frame_bytes, queue.front());
                queue.pop_front();
            }
        }
        if (written < wanted) {
            // Silence rather than a gap. An untouched buffer is a click, and a click is
            // worse than a moment of quiet -- and counting it says the caller fell behind.
            std::memset(data + static_cast<std::size_t>(written) * frame_bytes, 0,
                        static_cast<std::size_t>(wanted - written) * frame_bytes);
            silence += wanted - written;
            ++underruns;
        }
        render->ReleaseBuffer(wanted, 0);

        played += wanted;
        space.notify_all();
        drained.notify_all();
    }

    void Run() {
        // The playback thread touches COM objects, so it has to be in an apartment too.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop) {
            const DWORD waited = WaitForSingleObject(event, 100);
            if (stop) {
                break;
            }
            if (waited != WAIT_OBJECT_0) {
                continue;  // nothing to do yet; the loop re-checks `stop`
            }
            Pump();
        }
        CoUninitialize();
    }
};

WasapiSink::WasapiSink() {
    impl_ = std::make_unique<Impl>();
    Impl& impl = *impl_;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        error_ = "COM would not start (" + HResultText(com) + ")";
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(result)) {
        error_ = "there is no device enumerator (" + HResultText(result) + ")";
        return;
    }

    ComPtr<IMMDevice> device;
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.Put());
    if (FAILED(result)) {
        error_ = "there is no default output device (" + HResultText(result) + ")";
        return;
    }

    // The endpoint names itself by id; the friendly name would need the property store,
    // and an id is enough to tell one machine's devices apart in a diagnostic.
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
        device_name_ = [&] {
            std::string text;
            for (const wchar_t* at = id; *at != 0; ++at) {
                text.push_back(*at < 0x80 ? static_cast<char>(*at) : '?');
            }
            return text;
        }();
        CoTaskMemFree(id);
    }

    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(impl.client.Put()));
    if (FAILED(result)) {
        error_ = "the device would not open (" + HResultText(result) + ")";
        return;
    }

    WAVEFORMATEX* mix = nullptr;
    result = impl.client->GetMixFormat(&mix);
    if (FAILED(result) || mix == nullptr) {
        error_ = "the device would not say what format it wants (" + HResultText(result) + ")";
        return;
    }
    rate_ = mix->nSamplesPerSec;

    // Shared mode means feeding the device *its* format, so the only question is whether
    // this layer mixes in it. It does not resample, and says so instead of doing it.
    bool format_ok = mix->nChannels == audio::kChannels && mix->nSamplesPerSec == audio::kSampleRate;
    if (format_ok) {
        GUID subtype = {};
        std::uint16_t bits = mix->wBitsPerSample;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            subtype = extended->SubFormat;
            bits = extended->Format.wBitsPerSample;
        } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        } else if (mix->wFormatTag == WAVE_FORMAT_PCM) {
            subtype = KSDATAFORMAT_SUBTYPE_PCM;
        }

        if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && bits == 32) {
            impl.float32 = true;
            format_ok = true;
        } else if (IsEqualGUID(subtype, KSDATAFORMAT_SUBTYPE_PCM) && bits == 16) {
            impl.float32 = false;
            format_ok = true;
        } else {
            format_ok = false;
        }
    }
    if (!format_ok) {
        error_ = "the device is " + std::to_string(mix->nSamplesPerSec) + " Hz, " +
                 std::to_string(mix->nChannels) + " channel(s), " +
                 std::to_string(mix->wBitsPerSample) + " bit; this layer mixes " +
                 std::to_string(audio::kSampleRate) + " Hz stereo";
        CoTaskMemFree(mix);
        return;
    }
    impl.frame_bytes = static_cast<std::uint32_t>(mix->nBlockAlign);

    impl.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl.event == nullptr) {
        error_ = "the device event could not be created";
        CoTaskMemFree(mix);
        return;
    }

    // A tenth of a second of buffer, event-driven: the device calls, the thread fills.
    constexpr REFERENCE_TIME kBufferHundredNs = 1'000'000;
    result = impl.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     kBufferHundredNs, 0, mix, nullptr);
    if (FAILED(result)) {
        error_ = "the device would not start (" + HResultText(result) + ")";
        CoTaskMemFree(mix);
        return;
    }
    CoTaskMemFree(mix);

    result = impl.client->GetBufferSize(&impl.buffer_frames);
    if (FAILED(result)) {
        error_ = "the device would not say how big its buffer is (" + HResultText(result) + ")";
        return;
    }
    result = impl.client->SetEventHandle(impl.event);
    if (FAILED(result)) {
        error_ = "the device would not take the event (" + HResultText(result) + ")";
        return;
    }
    result = impl.client->GetService(__uuidof(IAudioRenderClient),
                                     reinterpret_cast<void**>(impl.render.Put()));
    if (FAILED(result)) {
        error_ = "the device has no render client (" + HResultText(result) + ")";
        return;
    }

    result = impl.client->Start();
    if (FAILED(result)) {
        error_ = "the device would not run (" + HResultText(result) + ")";
        return;
    }

    impl.worker = std::thread([&impl] { impl.Run(); });
    ok_ = true;
}

WasapiSink::~WasapiSink() {
    if (impl_ == nullptr) {
        return;
    }
    Impl& impl = *impl_;
    if (ok_) {
        Finish();
    }
    impl.stop = true;
    if (impl.event != nullptr) {
        SetEvent(impl.event);
    }
    if (impl.worker.joinable()) {
        impl.worker.join();
    }
    if (impl.client) {
        impl.client->Stop();
    }
    if (impl.event != nullptr) {
        CloseHandle(impl.event);
        impl.event = nullptr;
    }
    if (impl.failed && error_.empty()) {
        std::lock_guard<std::mutex> lock(impl.mutex);
        error_ = impl.failure;
    }
}

bool WasapiSink::Submit(std::span<const audio::Frame> frames) {
    if (impl_ == nullptr || !ok_) {
        return false;
    }
    Impl& impl = *impl_;
    std::unique_lock<std::mutex> lock(impl.mutex);
    std::size_t at = 0;
    while (at < frames.size()) {
        impl.space.wait(lock, [&] { return impl.queue.size() < kQueueFrames || impl.failed.load(); });
        if (impl.failed) {
            return false;
        }
        while (at < frames.size() && impl.queue.size() < kQueueFrames) {
            impl.queue.push_back(frames[at]);
            ++at;
        }
    }
    impl.submitted += frames.size();
    return true;
}

bool WasapiSink::Finish() {
    if (impl_ == nullptr || !ok_) {
        return false;
    }
    Impl& impl = *impl_;
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.drained.wait_for(lock, std::chrono::milliseconds(kDrainTimeoutMs),
                              [&] { return impl.played >= impl.submitted || impl.failed.load(); });
    }
    if (impl.failed) {
        return false;
    }
    // The device buffer itself is still playing out; stopping now would cut it off.
    const auto buffer_ms = static_cast<int>(impl.buffer_frames * 1000 /
                                            static_cast<std::uint64_t>(audio::kSampleRate));
    std::this_thread::sleep_for(std::chrono::milliseconds(buffer_ms));
    impl.client->Stop();
    return true;
}

std::uint64_t WasapiSink::frames_played() const noexcept {
    return impl_ == nullptr ? 0 : impl_->played.load();
}

std::uint64_t WasapiSink::silence_frames() const noexcept {
    return impl_ == nullptr ? 0 : impl_->silence.load();
}

std::uint64_t WasapiSink::underruns() const noexcept {
    return impl_ == nullptr ? 0 : impl_->underruns.load();
}

std::unique_ptr<audio::Sink> OpenDefaultDevice(std::string& error) {
    auto sink = std::make_unique<WasapiSink>();
    if (!sink->ok()) {
        error = sink->error();
        return nullptr;
    }
    error.clear();
    return sink;
}

}  // namespace zlong::host
