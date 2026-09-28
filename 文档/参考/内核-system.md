# 内核-system

# `system.h`

`内核/system/include/zlong/system/system.h`

```
烛龙 (ZhuLong) - the machine: the kernel at the top, owning the engine and the host.

内核/ had every part (RAM, GPU, audio, services) and 引擎/ had the engine, but nobody
owned them together: each host program assembled its own physical memory and GPU, and
the engine reached the GPU from a private arena that no allocator knew about. This is
the object that ends that. It constructs and holds the memory, the GPU, the render
backend, the audio device, the kernel and the engine -- and it is the one that drives
them.

The host does not assemble and does not run a loop. It implements Platform -- the
platform half (a window, a sound card, input) -- and hands it over; the frame loop is
here. Same shape as gpu::GpuHost and audio::Sink: the machine calls into the platform,
not the other way round.
```

```cpp
// What the machine drives on the host side. A host implements it and the System calls
// it. One object covers both halves -- a window and a sound card in a game, nothing at
// all in a headless run -- because a caller that wants only one half leaves the other's
// defaults alone.
class Platform {
    virtual ~Platform() = default;
    // Process pending platform work. False once the platform is gone (its window
    // closed), which stops the frame loop. A headless caller never sees this called.
    virtual bool Pump() { return true; }
    // A finished frame is ready: RGBA8, top row first, `pitch` bytes per row.
    virtual void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                         std::uint32_t height) {
    // Called once before each frame is rendered, with the previous frame's duration in
    // seconds. Where a host collects input and moves the camera; `scene` is the one
    // being drawn.
    virtual void OnFrame(engine::Scene& scene, double last_frame_seconds) {
    // Called once before each block is mixed. The sound half of OnFrame -- where a host
    // that animates sound does so; `scene` is the one being mixed, and `seconds_mixed`
    // is how far into the run this block starts.
    virtual void OnMix(engine::Scene& scene, double seconds_mixed) {
// The machine. Constructs every layer, owns them, and drives the loop.
class System final : public gpu::GpuHost {
    struct Config {
    // How a frame ended.
    enum class FrameResult {
    // Build the machine: memory, GPU, backend, kernel, engine. On failure `ok()` is
    // false and `error()` names the step; the object is still safe to destroy.
    explicit System(const Config& config);
    // --- what the machine is made of ----------------------------------------
    ram::Ram& ram() noexcept { return ram_; }
    gpu::Gpu& gpu() noexcept { return *gpu_; }
    service::Kernel& kernel() noexcept { return *kernel_; }
    engine::Renderer& renderer() noexcept { return *renderer_; }
    engine::SoundRenderer& sound() noexcept { return sound_; }
    const char* backend_name() const noexcept { return backend_->name(); }
    // False when the machine could not be built; `error()` says why.
    bool ok() const noexcept { return backend_ != nullptr; }
    // The reason the last call failed. Empty after a call that succeeded.
    const std::string& error() const noexcept { return error_; }
    // A non-fatal note from construction -- why a requested backend was not used, for
    // instance. Empty when there is nothing to say.
    const std::string& note() const noexcept { return note_; }
    // --- the host half -------------------------------------------------------
    // Borrow the platform the machine drives. Not owned; the host keeps it alive.
    void set_platform(Platform* platform) noexcept { platform_ = platform; }
    Platform* platform() const noexcept { return platform_; }
    // Borrow the audio device. It is wired into the kernel's services as well as the
    // engine, so a game and the engine are heard through the same sink.
    void set_audio(audio::Sink* sink) noexcept;
    audio::Sink* audio() const noexcept { return audio_; }
    // --- what it drives ------------------------------------------------------
    // Give the machine a scene: upload it, lay it out, keep it.
    //
    // The arena is reserved on the **first** call and reused by every later one, so loading
    // a scene that has changed re-uploads into the same bytes instead of taking another
    // slice of DRAM. That is what lets a host keep editing: build the scene, Load, draw a
    // frame, change the scene, Load again. It can only ever be reserved once -- the kernel's
    // allocator is a bump allocator, and a block it has handed out does not come back (see
    // `ram::PhysicalMemory::allocate`).
    bool Load(const engine::Scene& scene);
    engine::Scene& scene() noexcept { return scene_; }
    // One frame: the engine renders, the platform presents. `OnFrame` runs first, so a
    // host's input is already in the scene when it is drawn.
    FrameResult Frame();
    // Frame() until the platform is gone or `frames` have gone (negative: no limit).
    // Returns the number of frames drawn, or -1 when a frame failed -- `error()` then
    // says why.
    int Run(int frames);
    // Mix and send `seconds` of audio through the device. False on a mix or sink error.
    bool Mix(double seconds);
    // Tell the device no more frames are coming.
    bool FinishAudio();
    // --- gpu::GpuHost: what the device reports back --------------------------
    void OnGpuFault(const gpu::GpuFault& fault) override;
    void OnSyncpointSignal(gpu::SyncpointId id, std::uint32_t value) override;
    const std::vector<gpu::GpuFault>& fault_log() const noexcept { return fault_log_; }
```

---
