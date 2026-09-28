# 内核-audio

# `format.h`

`内核/audio/include/zlong/audio/format.h`

```
烛龙 (ZhuLong) - the sample format the audio layer works in.

One vocabulary for the whole layer, the way the GPU layer has one for surfaces:
what a sample is, what a frame is, and what rate they are at. The render path's
equivalent is surface/format.h.
```

```cpp
// Stereo, which is what the console's usual output is and what the engine's own
// sounds are authored for. A wider stream would change this one number and the
// sink that writes it.
inline constexpr std::uint32_t kChannels = 2;
// What a mix is accumulated in: float, so voices sum without clipping *inside* the
// sum and a gain of exactly one is exact. Quantising to something a device wants is
// the sink's business, and it is the only place a mix gets clamped.
using Sample = float;
// One instant across every channel, in channel order. Frames rather than interleaved
// samples, because a rate conversion reads one frame at a time.
using Frame = std::array<Sample, kChannels>;
// The rate everything is mixed at. A clip at another rate is converted on the way
// in, by the mixer.
inline constexpr std::uint32_t kSampleRate = 48000;
// PCM at its own rate.
struct Clip {
    bool empty() const noexcept { return frames.empty(); }
    // Whether it can be played at all: something to play, and a rate to play it at.
    bool Valid() const noexcept { return !frames.empty() && rate > 0; }
    // How long it lasts, in seconds.
    double Seconds() const noexcept {
// Clip frames per output frame: one when the rates match, and less when the clip is
// slower. A voice reads with this so that a clip lands at the pitch it was authored
// at rather than at whatever rate it happens to be stored in.
inline double StepFor(const Clip& clip) noexcept {
// One sample as signed sixteen-bit PCM: clamped to [-1, 1] and scaled by 32767, rounded
// to nearest. Clamping happens here and nowhere earlier -- a sum louder than one keeps
// its shape until this point -- and it lives beside the format rather than beside one
// sink because every sink that stores or plays integer PCM needs the same number.
inline std::int16_t ToPcm16(Sample sample) noexcept {
// The inverse: signed sixteen-bit PCM as the float the mixer carries.
//
// The scale is the same 32767 that ToPcm16 quantises with, and deliberately the same
// number, so a sample that arrives from a guest comes back out exactly as it came in.
// The one value that cannot is -32768, which lands one step above itself. It lives here
// rather than beside a service because every path a guest's PCM takes in needs it.
inline Sample FromPcm16(std::int16_t value) noexcept {
```

---

# `mixer.h`

`内核/audio/include/zlong/audio/mixer.h`

```
烛龙 (ZhuLong) - summing voices into frames.

What the software rasteriser is to the render path: the thing that turns a
description into the samples themselves. A voice reads a clip at the clip's own
rate and writes into the mix at the output's, so the rate conversion lives here and
nothing above has to know about it.
```

```cpp
// One clip being mixed.
struct Voice {
// Fills `block` with the sum of `voices` and advances every voice that has not
// finished. The block is *replaced*, not added to, so the same buffer can be handed
// to a sink afterwards.
void Mix(std::span<Voice> voices, std::span<Frame> block);
```

---

# `sink.h`

`内核/audio/include/zlong/audio/sink.h`

```
烛龙 (ZhuLong) - where mixed frames go.

The render path's equivalent of RenderBackend: the mixer produces frames and
something outside it decides what becomes of them. Keeping that seam here is what
lets the same mix be written to a file, played on a device, or counted by a test.
```

```cpp
class Sink {
    virtual ~Sink() = default;
    // Takes `frames`. Returns false when the sink could not take them -- a device
    // that has gone away, a file that cannot be written.
    virtual bool Submit(std::span<const Frame> frames) = 0;
    // Called once, when no more frames are coming, so a sink that has to finish
    // something off (a file header, a device buffer) can. The render path's `Flush`
    // is the same seam.
    virtual bool Finish() { return true; }
    // What to call this sink in a diagnostic.
    virtual const char* name() const noexcept = 0;
```

---

# `wav_sink.h`

`内核/audio/include/zlong/audio/wav_sink.h`

```
烛龙 (ZhuLong) - a sink that writes a sixteen-bit PCM WAV file.

The audio path's equivalent of rendering into a bitmap: deterministic, needs no
device, and something a regression test can compare byte for byte.
```

```cpp
class WavSink final : public Sink {
    // Opens `path` for writing and lays down a header it will fill in at the end. A
    // failure is reported through `ok()` rather than thrown, the way the backends
    // report `Initialize`.
    explicit WavSink(std::string path, std::uint32_t rate = kSampleRate);
    bool Submit(std::span<const Frame> frames) override;
    // Rewrites the header with the lengths now known. Idempotent.
    bool Finish() override;
    const char* name() const noexcept override { return "wav"; }
    bool ok() const noexcept { return file_ != nullptr && !failed_; }
    // Why it failed, for a diagnostic. Empty while `ok()`.
    const std::string& error() const noexcept { return error_; }
    bool WriteHeader();
```

---
