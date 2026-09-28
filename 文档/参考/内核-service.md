# 内核-service

# `address_space.h`

`内核/service/include/zlong/service/address_space.h`

```
烛龙 (ZhuLong) - the guest address space, as the kernel sees it.

RAM's AddressSpace builds page tables. It cannot answer the questions a
syscall has to answer -- "is this range mine", "is it already mapped", "what
is it allowed to be used for" -- because a page table only stores the last of
those. This is the layer that tracks the regions, so a mapping request can be
checked against what the process actually owns.

One address space, not one per process: under HLE the guest's own EL1 kernel
does not run, so nothing switches regimes. That matches how the RAM layer
already treats the translation control registers.
```

```cpp
// What a mapped range may be used for.
inline constexpr std::uint32_t kMemoryRead = 1u << 0;
inline constexpr std::uint32_t kMemoryWrite = 1u << 1;
inline constexpr std::uint32_t kMemoryExecute = 1u << 2;
// Where guest virtual addresses start. Chosen clear of the low mappings a guest
// might still make, and inside the 48-bit range a t0sz of 16 covers.
inline constexpr std::uint64_t kFirstGuestAddress = 0x0000'0000'1000'0000ull;
// Anything at or above this is outside what the configured regime translates.
inline constexpr std::uint64_t kLastGuestAddress = 0x0000'7FFF'FFFF'FFFFull;
class KAddressSpace {
    // Point the MMU at a fresh root table and turn it on. Idempotent.
    bool Enable();
    struct Region {
    // Set a virtual range aside with no backing yet. Refuses an overlap, an
    // unaligned or empty range, and one outside the translatable range.
    bool Reserve(std::uint64_t base, std::uint64_t size, std::string& error);
    // Give a reserved range physical pages and map it. Refuses a range that was
    // never reserved, or one that is already mapped.
    bool Map(std::uint64_t base, std::uint64_t size, std::uint32_t permissions,
             std::string& error);
    // Map a reserved range onto a physical range the caller already owns.
    //
    // Boot uses this to place the entry module at a known physical address, and
    // a test uses it to keep the code it is running mapped once the MMU is on.
    bool MapTo(std::uint64_t base, std::uint64_t physical_base, std::uint64_t size,
               std::uint32_t permissions, std::string& error);
    // Reserve and map `size` bytes at `base`, then copy `bytes` into them. The
    // last page is padded, so `size` need not be a multiple of the page size.
    //
    // The copy goes through the translation this just installed, at EL1, and
    // writes the physical page directly. That is deliberate: a loader fills a
    // page it has mapped read-only or execute-only, and a store from EL0 would
    // -- correctly -- fault on it.
    //
    // The physical pages come from the allocator, so the image cannot land on
    // the kernel's page tables. See PhysicalMemory::allocate for why a
    // hard-coded physical address can.
    //
    // Writes the physical page directly, which means it does NOT report the
    // write as guest code, so a core that had already translated this memory
    // would keep running its old translation. Boot loads before any core runs.
    // Loading over memory a core has executed needs the guest write path
    // instead (Ram::TryWrite*, which reaches the code-write observer).
    bool LoadImage(std::uint64_t base, const void* bytes, std::size_t size,
                   std::uint32_t permissions, std::string& error);
    // Drop a mapping.
    //
    // The physical pages are NOT handed back: the physical allocator is a bump
    // allocator with no free. That is a real leak, it is bounded by how often a
    // guest maps and unmaps, and it is written down here rather than left to be
    // discovered.
    bool Unmap(std::uint64_t base, std::uint64_t size, std::string& error);
    // Change the permissions of a mapped range.
    bool SetAttributes(std::uint64_t base, std::uint64_t size, std::uint32_t permissions,
                       std::string& error);
    const Region* Find(std::uint64_t address) const;
    const std::vector<Region>& regions() const noexcept { return regions_; }
    bool enabled() const noexcept { return enabled_; }
    // The single region that wholly contains [base, base + size).
    Region* FindContaining(std::uint64_t base, std::uint64_t size);
    // The checks both Map and MapTo need before either touches a page table.
    Region* PrepareMapping(std::uint64_t base, std::uint64_t size, std::string& error);
```

---

# `boot.h`

`内核/service/include/zlong/service/boot.h`

```
烛龙 (ZhuLong) - bringing up the first process.

Boot is the one place that knows the order: a process, the address space it
runs in, the module loaded into it, and the main thread that starts at its
entry point. Everything it builds already exists -- this is the sequence, not
a new mechanism.

It deliberately does NOT start a core. A host decides when cores run, and a
test drives one synchronously from the entry point. See Kernel::ReadyThread.
```

```cpp
class Core;
// A module to run: the bytes, where they go, and where execution starts.
struct ModuleImage {
struct BootConfig {
// A process that has been built, with a main thread ready to run.
//
// The caller owns both, and has to keep them alive for as long as the kernel
// may reach them: the kernel's thread registry only borrows (see
// Kernel::BindThread). Unbind before letting either go.
struct BootedProcess {
```

---

# `handle.h`

`内核/service/include/zlong/service/handle.h`

```
烛龙 (ZhuLong) - the handle table.

A handle carries a slot index *and* a generation. The generation is not an
optimisation: without it, closing a handle and letting the slot be reused
makes the stale handle alias whatever object landed there next -- a silent
wrong answer of exactly the kind this kernel refuses to give. With it, a stale
handle always misses.

The generation is 16 bits, so after 65536 close/reuse cycles on one slot an
ancient handle could match again. That window is documented rather than
closed: closing it would mean never reusing slots, which leaks a table entry
per handle ever handed out.

The table is not internally locked. One table belongs to one process, and the
process's own lock is the caller's business -- the service layer takes it at
the syscall boundary, once, rather than per lookup.
```

```cpp
using Handle = std::uint32_t;
// Zero is never a valid handle, so a zeroed structure cannot name an object.
inline constexpr Handle kInvalidHandle = 0;
inline constexpr std::uint32_t kHandleIndexMask = 0xFFFF;
// One slot index per handle; index 0 is reserved as "invalid".
inline constexpr std::uint32_t kMaxHandles = kHandleIndexMask;
class HandleTable {
    // Hand out a handle for `object`, taking a reference. Returns
    // kInvalidHandle when the object is null or the table is full.
    Handle Allocate(KObject* object);
    // The object a handle names, or nullptr -- for a dead handle, a handle
    // never handed out, or one whose generation no longer matches.
    KObject* Get(Handle handle) const noexcept;
    // As Get, but only when the object's kind matches.
    template <typename T>
    T* GetAs(Handle handle) const noexcept {
    T* GetAs(Handle handle) const noexcept {
    // Drop the handle's reference. False when the handle was already dead.
    bool Close(Handle handle);
    // Close every handle. Used when the owning process dies, and by the
    // destructor, so no reference is ever leaked by forgetting a close.
    void CloseAll();
    struct Slot {
    // Retire the slot at `index`: bump its generation so every handle that
    // named the old object misses, then make the slot available again.
    void Retire(std::size_t index) noexcept;
```

---

# `ipc.h`

`内核/service/include/zlong/service/ipc.h`

```
烛龙 (ZhuLong) - IPC: the command buffer, and the boundary it sits on.

This is where guest-supplied pointers and lengths arrive, so it is the one
place in the kernel that must treat everything as hostile. Every field is read
through GuestMemory, which reports failure instead of throwing, and NOTHING is
returned half-unpacked: a request that could not be read in full comes back
empty with a reason. A partially filled request would let a service act on
garbage while looking like it had been given data.

============================================================================
LAYOUT: this command buffer is THIS PROJECT'S PLACEHOLDER, like the syscall
numbers. The console's real layout has to be transcribed. It lives here so
replacing it is one edit.

offset 0    u32  magic         'ZLIP'
offset 4    u32  command_id
offset 8    u32  data_size
offset 12   u32  buffer_count
offset 16   data[data_size]
then        buffer_count x { u64 address; u64 size; u32 mode; u32 reserved }

A response is the same header with `command_id` replaced by `result` and no
buffer table:

offset 0    u32  magic
offset 4    u32  result
offset 8    u32  data_size
offset 12   u32  reserved
offset 16   data[data_size]
============================================================================
```

```cpp
inline constexpr std::uint32_t kIpcMagic = 0x5A4C'4950u;  // 'ZLIP'
inline constexpr std::size_t kIpcHeaderBytes = 16;
inline constexpr std::size_t kIpcBufferEntryBytes = 24;
// Bounds chosen so a corrupt header cannot ask for a huge allocation before any
// range check has run.
inline constexpr std::uint32_t kMaxIpcDataBytes = 1u << 20;
inline constexpr std::uint32_t kMaxIpcBuffers = 64;
inline constexpr std::uint32_t kIpcBufferModeNormal = 0;
// A buffer the guest described. Held as an address, not a pointer: the handler
// may park, and a guest address stays meaningful where a host pointer would not.
struct IpcBuffer {
struct IpcRequest {
struct IpcResponse {
// Returned in a response when a service does not know the command. Nonzero, for
// the same reason an SVC without a handler is nonzero.
inline constexpr std::uint32_t kIpcResultUnknownCommand = 0x5A1D'0002u;
enum class IpcError {
const char* ToString(IpcError error) noexcept;
// Pull a request out of guest memory. On failure `request` is left empty.
bool UnpackRequest(cpu::GuestMemory& memory, std::uint64_t address, IpcRequest& request,
                   IpcError* error = nullptr);
// Write a response into `room` bytes at `address`. Refuses rather than
// truncating: a truncated response is a lie about what the service said.
bool PackResponse(cpu::GuestMemory& memory, std::uint64_t address, std::uint64_t room,
                  const IpcResponse& response, IpcError* error = nullptr);
// The bytes a response of this size needs.
constexpr std::uint64_t ResponseBytes(std::uint64_t data_size) noexcept {
```

---

# `kernel.h`

`内核/service/include/zlong/service/kernel.h`

```
烛龙 (ZhuLong) - the kernel: the CpuHost the CPU layer calls into.

The CPU layer holds no syscall numbers, no HLE and no guest exception model:
it reports events and this decides what they mean. OnSvc is the front door.

OnInterrupt is the other one, and it is honest about being unfinished: guest
exception entry is not implemented, so an interrupt does not reach the guest.
That gap is counted rather than hidden -- see the note on the implementation.
```

```cpp
class Gpu;
class Sink;
class NvDrvService;
class AudOutService;
class AudRenService;
class Kernel final : public cpu::CpuHost {
    // The RAM layer is needed for the address space: the memory syscalls manage
    // it, and boot maps the entry module through it.
    explicit Kernel(ram::Ram& ram);
    ram::Ram& ram() noexcept { return ram_; }
    KAddressSpace& address_space() noexcept { return address_space_; }
    const KAddressSpace& address_space() const noexcept { return address_space_; }
    SyscallTable& syscalls() noexcept { return syscalls_; }
    const SyscallTable& syscalls() const noexcept { return syscalls_; }
    void OnSvc(cpu::Core& core, const cpu::SvcCall& call) override;
    void OnInterrupt(cpu::Core& core, cpu::Interrupt interrupt) override;
    // Bind a thread to the core it runs on. False when the waiter id is already
    // bound to a different thread.
    bool BindThread(KThread& thread, cpu::Core& core);
    bool UnbindThread(WaiterId waiter);
    KThread* FindThread(WaiterId waiter) const;
    // The thread bound to this core, or nullptr. A scan, because there are a
    // handful of threads and a second index would be one more thing to keep
    // consistent.
    KThread* ThreadOn(cpu::Core& core) const;
    // Wake whoever `waiter` names. False when the id is unknown or the thread
    // has no core -- a waiter that cannot be reached is a bug, not a no-op, so
    // it is reported rather than swallowed.
    bool Wake(WaiterId waiter);
    void ResetStrandedWakes() noexcept { stranded_wakes_.store(0, std::memory_order_relaxed); }
    // The service set. Populated before boot: the kernel does not own which
    // services exist, only that there is one place they are found.
    ServiceRegistry& services() noexcept { return services_; }
    const ServiceRegistry& services() const noexcept { return services_; }
    // Wire the GPU submission path: everything nvdrv submits goes here, and what
    // the GPU reports back goes to the Gpu's own host. Until this is called,
    // nvdrv exists and refuses every submission with a reason.
    void attach_gpu(gpu::Gpu& gpu) noexcept;
    // The wired GPU, or nullptr.
    gpu::Gpu* gpu() const noexcept;
    // Wire the audio device: everything audout's guest hands over, and everything
    // audren mixes, goes here. Both services get it, so a game is heard through the same
    // sink whether it appends finished PCM or registers voices. Until this is called,
    // both refuse with a reason, so a guest is told nothing took its audio.
    void attach_audio(audio::Sink& sink) noexcept;
    // The wired device, or nullptr.
    audio::Sink* audio_sink() const noexcept;
    // Hand out a thread id. The kernel owns the numbering so that a tid is
    // never reused while a stale waiter could still name it.
    WaiterId AllocateTid() noexcept {
    // Start a thread the guest created, on the core it was bound to. False when
    // the handle names nothing, is not a thread, is already started, or the
    // core is gone. Public for the same reason as SignalEventHandle.
    bool StartThreadHandle(Handle handle);
    // Program a thread's recorded entry state onto its core -- PC, SP and the
    // first argument register -- and mark it ready. Does NOT start the core: a
    // host decides when cores run, and a test drives one synchronously from the
    // entry point instead. Boot readies a main thread this way.
    //
    // False when the thread has already left Created, or has no core.
    bool ReadyThread(KThread& thread);
    // Stop the calling thread: mark it finished and stop its core. False when
    // no thread is bound to that core.
    bool ExitCurrentThread(cpu::Core& core);
    // Signal the event a handle names and wake whoever it released. Public
    // because the same pair is needed outside a syscall -- a test now, a device
    // later -- and duplicating it would be two places to get the lock order
    // wrong. False when the handle names nothing or is not an event.
    bool SignalEventHandle(Handle handle);
    // The process whose handle table syscalls resolve against. Set at boot.
    void set_process(KProcess* process) noexcept { process_ = process; }
    KProcess* process() const noexcept { return process_; }
```

---

# `object.h`

`内核/service/include/zlong/service/object.h`

```
烛龙 (ZhuLong) - the kernel object base.

Everything a guest can hold a handle to derives from KObject. The base carries
exactly two things: a type tag the handle table checks, and a reference count.

Lifetime is by reference count alone -- there is no ownership tree. An object
outlives the process that created it in every design that works, so ownership
would only add a second, disagreeing notion of when an object dies.
```

```cpp
// The type tag a handle table checks against. A mismatch is a miss, never a
// reinterpretation.
enum class ObjectKind : std::uint8_t {
const char* ToString(ObjectKind kind) noexcept;
class KObject {
    virtual ~KObject() = default;
    ObjectKind kind() const noexcept { return kind_; }
    // Take a reference. Called by whoever hands the object out.
    void AddRef() noexcept;
    // Drop a reference. Returns true when that call destroyed the object.
    bool Release() noexcept;
    // Compile-time-checked type test, for a type that declares `kKind`.
    template <typename T>
    bool is() const noexcept {
    bool is() const noexcept {
    explicit KObject(ObjectKind kind) noexcept : kind_(kind) {}
```

---

# `process.h`

`内核/service/include/zlong/service/process.h`

```
烛龙 (ZhuLong) - a guest process.

A process owns a handle table and the view of the mounted sources its threads
resolve paths through. It owns no memory: the address space belongs to the RAM
layer, and the process only refers to it -- which is what keeps this layer
free of page-table knowledge.
```

```cpp
class KAddressSpace;
class KThread;
class KProcess final : public KObject {
    static constexpr ObjectKind kKind = ObjectKind::Process;
    explicit KProcess(std::uint64_t pid) noexcept;
    HandleTable& handles() noexcept { return handles_; }
    const HandleTable& handles() const noexcept { return handles_; }
    // Install the mounted sources. Set once, before the process runs, and never
    // changed afterwards -- so readers need no lock for it.
    void set_mounts(const mount::MountTable* mounts) noexcept { mounts_ = mounts; }
    const mount::MountTable* mounts() const noexcept { return mounts_; }
    // The address space the process's threads run in. Set once at boot, and
    // never changed: under HLE the guest's own EL1 kernel does not run, so there
    // is one regime and nothing switches it.
    //
    // Borrowed: the kernel owns the address space and outlives any process. The
    // process only says which one it runs in, which is what keeps this layer
    // free of page-table knowledge.
    void set_address_space(KAddressSpace* space) noexcept { address_space_ = space; }
    KAddressSpace* address_space() const noexcept { return address_space_; }
    // The thread the process starts on. Set once at boot.
    //
    // Borrowed on the same contract as the kernel's waiter registry: the owner
    // keeps it alive and has to unbind before it dies. Nothing here takes a
    // reference, because a thread that outlived its process would be a worse
    // lie than a documented contract.
    void set_main_thread(KThread* thread) noexcept { main_thread_ = thread; }
    KThread* main_thread() const noexcept { return main_thread_; }
    bool terminated() const noexcept { return terminated_.load(std::memory_order_acquire); }
    // Mark the process dead and close every handle it holds. Idempotent: the
    // first call wins and the rest are no-ops.
    void Terminate() noexcept;
```

---

# `registry.h`

`内核/service/include/zlong/service/registry.h`

```
烛龙 (ZhuLong) - the service registry.

A name maps to a service. Connecting creates a session; a session serialises
its requests, so a service is never entered on two cores at once. That is why
HandleRequest may touch plain members without a lock of its own.
```

```cpp
class Core;
class Kernel;
// A service name is read out of guest memory, so its length is bounded here
// rather than trusted.
inline constexpr std::size_t kMaxServiceNameBytes = 64;
// What a service is told about the caller.
//
// A service is a free-standing object with no chain of `this` back to the
// kernel, and it has no state of its own to fall back on: resolving a path
// needs the caller's mounts, reading the clock needs the caller's core. So
// whatever it needs is handed over per request -- the same reasoning as
// SvcContext, one layer down.
//
// The session is here for identity. `kernel.process()` is the process that
// request resolves against, which is the caller today and will need the
// session to tell apart once there is more than one.
struct ServiceContext {
class IService {
    virtual ~IService() = default;
    virtual const char* name() const noexcept = 0;
    // Serve one request. The session is already serialised against every other
    // sender, so only one core is ever inside this.
    virtual void HandleRequest(ServiceContext& context, const IpcRequest& request,
                               IpcResponse& response) = 0;
class ServiceRegistry {
    // Register under `name`. Refuses an empty name or a duplicate: letting one
    // silently shadow another would make a connect land somewhere unexpected.
    bool Register(std::string name, std::unique_ptr<IService> service);
    // The service registered under `name`, or nullptr.
    IService* Find(std::string_view name) const;
    struct Entry {
```

---

# `audout_service.h`

`内核/service/include/zlong/service/services/audout_service.h`

```
烛龙 (ZhuLong) - audout: the guest's path to the audio device.

The console's real service is `audout:u`: open an output, start it, append buffers of
PCM, and be told by an event when one has played. The command ids and buffer structs
below are THIS PROJECT'S PLACEHOLDER, like every other layout in this layer.

What is not a placeholder is where the samples end up. They go to an `audio::Sink` --
the same seam the engine's own mixer writes to -- so a game's audio and the engine's
take the same road out, and a test can count what a guest sent without a sound card.

Two things are deliberately narrower than the console, and say so rather than
pretending:

* the device is 48 kHz stereo, which is what this layer's mixer works at. An output
asking for another rate or channel count is refused with a reason, not quietly
resampled behind the guest's back.
* a buffer is drained into the sink on the call that appends it, so it is released
immediately and the event is signalled straight away. There is no device thread
yet; a real one would hold the buffer until it had actually played.
```

```cpp
class AudOutService final : public IService {
    // Commands, in the guest's command buffer. Placeholder ids.
    enum Command : std::uint32_t {
    enum class State : std::uint32_t { Stopped = 0, Started = 1 };
    const char* name() const noexcept override { return "audout"; }
    // Wire the device in. Borrowed, and may be null: with nothing behind it an append
    // is refused with a reason rather than dropping the guest's audio on the floor.
    void set_sink(audio::Sink* sink) noexcept { sink_ = sink; }
    audio::Sink* sink() const noexcept { return sink_; }
    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;
    // The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }
    // One open output: the console's IAudioOut, minus the parts not modelled.
    struct Output {
    void Open(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void Append(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void TakeReleased(const IpcRequest& request, IpcResponse& response);
    void SetState(const IpcRequest& request, IpcResponse& response, State state);
    void ReportState(const IpcRequest& request, IpcResponse& response);
    Output* Find(std::uint32_t id);
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);
```

---

# `audren_service.h`

`内核/service/include/zlong/service/services/audren_service.h`

```
烛龙 (ZhuLong) - audren: the guest's mixer.

Where audout is handed a buffer of finished PCM and passes it to the device, audren
is handed *voices* -- a sample address, a rate, a pitch, a gain -- and produces the
mix itself. That is the whole difference, and it is why this service reaches for the
audio layer's mixer rather than only a sink: `audio::Voice` and `audio::Mix` already
mean "sum these clips into frames, converting rate on the way in", which is what the
console's renderer does with far more knobs than are modelled here.

The command ids and the voice descriptor below are THIS PROJECT'S PLACEHOLDER, like
every other layout in this layer. The console's `audren:u` takes a config struct of
several hundred bytes per voice, an effect graph, and a revision number.

Three things are deliberately narrower than the console, and say so rather than
pretending:

* the device is 48 kHz stereo, the rate this layer's mixer works at. A renderer
asking for another rate is refused with a reason, not quietly resampled.
* a voice's samples are read out of guest memory once, by the call that registers
it, and the service owns the copy from then on. The console re-reads the wave
buffer every update, which is what lets a guest stream into a ring buffer; here
a guest that streams has to register again to be heard, and registering restarts
that voice's cursor. Re-reading per update would let the samples change under a
cursor that had already advanced, which is harder to be right about than a
documented limit.
* there is no effect graph and no worker thread. An update mixes the frames the
guest asks for and returns; nothing renders ahead of a request.

What is NOT narrowed is where the frames go. They land in the guest's own output
buffer, because that is what makes this a renderer rather than a player -- and, when
a device is attached, the same `audio::Sink` audout and the engine's mixer write to,
so a game that uses the renderer is heard without a second path out.
```

```cpp
class AudRenService final : public IService {
    // Commands, in the guest's command buffer. Placeholder ids.
    enum Command : std::uint32_t {
    // The most frames one update may ask for: a second at the device rate. A bound
    // rather than a promise -- it keeps the scratch block a guest-sized number.
    static constexpr std::uint32_t kMaxFramesPerUpdate = audio::kSampleRate;
    // The most voices a renderer holds.
    static constexpr std::uint32_t kMaxVoices = 64;
    // The most bytes one voice's samples may take. Sixteen mebibytes is about eighty
    // seconds of stereo at the device rate, and it is a bound for the same reason.
    static constexpr std::uint32_t kMaxClipBytes = 16u * 1024u * 1024u;
    // One descriptor: seven words.
    static constexpr std::size_t kDescriptorWords = 7;
    const char* name() const noexcept override { return "audren"; }
    // Wire the device in. Borrowed, and may be null: with nothing behind it an update
    // still renders into the guest's buffer, it is just not played.
    void set_sink(audio::Sink* sink) noexcept { sink_ = sink; }
    audio::Sink* sink() const noexcept { return sink_; }
    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;
    // The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }
    // One open renderer: what the console calls an audio renderer, minus the effects.
    struct Renderer {
    void Open(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void SetVoices(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void Update(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void Close(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    Renderer* Find(std::uint32_t id);
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);
```

---

# `debug_service.h`

`内核/service/include/zlong/service/services/debug_service.h`

```
烛龙 (ZhuLong) - the first service.

Deliberately trivial: it exists so the IPC path has somewhere to arrive, and so
a break anywhere along that path shows up here as a missing call instead of as
silence.

It counts what it served, which is what lets a test tell "the service did the
work" apart from "something happened to fill the buffer in".
```

```cpp
class DebugService final : public IService {
    static constexpr std::uint32_t kCommandPing = 1;
    static constexpr std::uint32_t kCommandEcho = 2;
    // This project's own answer to Ping, chosen so it cannot be confused with a
    // value that happened to be lying in the buffer.
    static constexpr std::uint32_t kPingAnswer = 0x5A1D'0001u;
    const char* name() const noexcept override { return "debug"; }
    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;
```

---

# `fsp_service.h`

`内核/service/include/zlong/service/services/fsp_service.h`

```
烛龙 (ZhuLong) - fsp-srv: the guest's filesystem.

This is where the mount layer becomes something a guest can use. A path is
addressed as "<mount>/<path inside that tree>" -- the same shape
mount::MountTable::Resolve takes, and the first segment is what picks the
tree. So "/system/abc" reads abc out of whatever is mounted as "system".

============================================================================
PLACEHOLDER, like every other command layout in this layer: the command ids
below, and the way a path and a byte range travel in the request's data. The
console's protocol -- its own request structs, its handle semantics, its error
codes -- is not transcribed.
============================================================================
```

```cpp
class FspService final : public IService {
    enum Command : std::uint32_t {
    // What kind of node a path names, as reported in a response.
    enum EntryKind : std::uint32_t { kEntryFile = 0, kEntryDirectory = 1 };
    const char* name() const noexcept override { return "fsp-srv"; }
    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;
    // The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }
    // An open file. The path is kept rather than a tree pointer: a file is a
    // range of a tree that a later unmount could replace, and re-resolving on
    // every read is what makes that show up as a refusal instead of as a read
    // from a tree that is no longer mounted.
    struct File {
    void GetMounts(ServiceContext& context, IpcResponse& response);
    void OpenFile(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void ReadFile(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void CloseFile(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void GetEntryType(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);
```

---

# `nvdrv_service.h`

`内核/service/include/zlong/service/services/nvdrv_service.h`

```
烛龙 (ZhuLong) - nvdrv: the guest's path to the GPU.

The console's real driver is a set of /dev/nvhost-* nodes and a pile of _IOWR
ioctls. Two things below are THIS PROJECT'S PLACEHOLDER, stated rather than
left to be inferred:

* the command ids, like every other IPC command id in this layer;
* the ioctl numbers, and the shape of a request's data (a word for the fd, a
word for the ioctl, then that ioctl's own words). The console's request
structs, and the device/ioctl matrix that says which ioctl each node
accepts, are not transcribed.

What is NOT a placeholder is the shape of the path: open a device, open a
channel on it, give the channel a command buffer, submit it, and have the
submission advance a syncpoint. Every one of those steps can refuse, and says
why rather than succeeding quietly.
```

```cpp
class Gpu;
inline constexpr const char* kNvHostCtrl = "/dev/nvhost-ctrl";
inline constexpr const char* kNvHostCtrlGpu = "/dev/nvhost-ctrl-gpu";
inline constexpr const char* kNvHostGpu = "/dev/nvhost-gpu";
inline constexpr const char* kNvMap = "/dev/nvmap";
// The first fd handed out. Zero is never a valid fd here, so a guest that
// forgot to read the out-parameter cannot look like it opened something.
inline constexpr std::uint32_t kNvDrvFirstFd = 1;
class NvDrvService final : public IService {
    // Commands, in the guest's command buffer.
    enum Command : std::uint32_t {
    // Ioctls this service implements. Placeholder numbers.
    enum Ioctl : std::uint32_t {
    // Ioctls /dev/nvmap takes. Placeholder numbers, in the same family as the
    // channel ones, and deliberately separate from them: the console's per-node
    // ioctl matrix is not transcribed, and a mapper that accepted channel ioctls
    // would only hide that.
    enum NvMapIoctl : std::uint32_t {
    const char* name() const noexcept override { return "nvdrv"; }
    // Wire the submission path in. Borrowed, and may be null: without it a
    // submission is refused with a reason instead of quietly succeeding. The
    // syncpoint signal goes to the Gpu's own host, so this is the same object
    // that receives the GPU's faults.
    void set_gpu(gpu::Gpu* gpu) noexcept { gpu_ = gpu; }
    gpu::Gpu* gpu() const noexcept { return gpu_; }
    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;
    // What an nvmap handle names, for tests and diagnosis. `physical` is what the
    // guest never has to see; `gpu_address` is what it does.
    struct NvMapAllocation {
    // The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }
    struct Device {
    struct Channel {
    void Open(const IpcRequest& request, IpcResponse& response);
    void Close(const IpcRequest& request, IpcResponse& response);
    void RunIoctl(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void OpenChannel(std::uint32_t device_fd, IpcResponse& response);
    void BindGpfifo(const IpcRequest& request, IpcResponse& response);
    void SubmitGpfifo(const IpcRequest& request, IpcResponse& response);
    void CloseChannel(const IpcRequest& request, IpcResponse& response);
    void WaitSyncpoint(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void RunNvMapIoctl(const IpcRequest& request, IpcResponse& response);
    void AllocateMemory(const IpcRequest& request, IpcResponse& response);
    void FreeMemory(const IpcRequest& request, IpcResponse& response);
    void ExposeForGpu(const IpcRequest& request, IpcResponse& response);
    // Record that a syncpoint has reached `value`, and wake every core waiting for
    // it. Called on the render worker, so it may not touch anything that belongs to
    // the submitting core.
    void AdvanceSyncpoint(gpu::SyncpointId id, std::uint32_t value);
    // Set a failure result and its reason, and report the refusal to the caller
    // by returning false.
    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);
    // The channel a word in a request names, or null when the fd is not an open
    // channel.
    Channel* FindChannel(std::uint32_t fd);
```

---

# `time_service.h`

`内核/service/include/zlong/service/services/time_service.h`

```
烛龙 (ZhuLong) - time.

What this can honestly answer is an interval: the CPU's counter, and the
frequency it ticks at. That is what a guest measuring elapsed time needs, and
it is real rather than invented.

What it CANNOT answer is a time of day. A wall clock needs an epoch this
layer does not have -- nothing has told it what the date is -- and making one
up would put a wrong date in the guest's hands with nothing to say so. So the
date commands are simply not here, and a guest that asks for one gets the
nonzero "unknown command" result instead of a plausible-looking number.

PLACEHOLDER, like every other command layout in this layer: the command ids
below are ours, not the console's.
```

```cpp
class TimeService final : public IService {
    enum Command : std::uint32_t {
    const char* name() const noexcept override { return "time"; }
    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;
```

---

# `session.h`

`内核/service/include/zlong/service/session.h`

```
烛龙 (ZhuLong) - a connection to a service.

A session is what an IPC request travels on, and it serialises those requests.
Two cores sending on one session must not run the handler at the same time: a
service that is not written to be re-entrant would corrupt its own state, and
nothing would say so. The serialisation lives here rather than in each service
so it cannot be forgotten.

A blocked sender parks and retries, exactly like every other wait in this
layer: BeginRequest returns whether the caller must park, and a woken caller
re-runs it from the top.
```

```cpp
// Implemented by the service registry. Forward-declared so a session does not
// have to pull the registry's interface in.
class IService;
class KSession final : public KObject {
    static constexpr ObjectKind kKind = ObjectKind::Session;
    const std::string& service_name() const noexcept { return service_name_; }
    // Attach the service this session talks to. Set once, at connect time,
    // before the session is handed to the guest.
    void Attach(IService* service) noexcept;
    IService* service() const;
    // True when `waiter` may send now; false when another request is in flight,
    // in which case `waiter` was registered and must park and retry.
    bool BeginRequest(WaiterId waiter);
    // Drop a registration that will not park.
    bool CancelRequestWait(WaiterId waiter);
    bool request_in_flight() const;
```

---

# `svc.h`

`内核/service/include/zlong/service/svc.h`

```
烛龙 (ZhuLong) - SVC dispatch.

The guest reaches the kernel through one door: an SVC. This is the table
behind it and the context a handler works with.

Two rules, both about not lying to the guest:

1. An SVC with no handler returns a NONZERO result and is counted. Returning
zero -- the success code -- would tell the guest a call it never made
succeeded, and the failure would surface somewhere unrelated much later.

2. A handler that cannot do the work says so: it calls Fail(), which sets a
nonzero result and a reason. The dispatcher never converts a failure into
a success, and never guesses a "reasonable" return value.

============================================================================
SVC NUMBERS: the enum below is THIS PROJECT'S PLACEHOLDER numbering, not the
console's. The real numbers must be transcribed from the console's syscall
list; until then they are deliberately parked in a block of our own so they
cannot be mistaken for real ones. They live here so that replacing them is one
edit, and nothing else in the tree spells a syscall number out.
============================================================================
```

```cpp
// The dispatcher. Forward-declared so a context can name it without this
// header pulling the kernel in.
class Kernel;
// Returned in x0 when an SVC has no handler. Nonzero and distinct on purpose:
// "not implemented" must not be mistakable for success.
inline constexpr std::uint64_t kResultNotImplemented = 0x5A5A'0001ull;
// The syscalls this layer knows about.
enum class Svc : std::uint32_t {
constexpr std::uint32_t SvcNumber(Svc id) noexcept { return static_cast<std::uint32_t>(id); }
// Event creation flags. PLACEHOLDERS, like the syscall numbers above: the
// console's encoding has to be transcribed. Kept next to them so replacing
// them is one edit.
inline constexpr std::uint64_t kEventManual = 1ull << 0;
inline constexpr std::uint64_t kEventSignalled = 1ull << 1;
// A failure reason. A literal, not a string: a syscall must not allocate.
using SvcFailure = const char*;
struct SvcContext {
    void SetResult(std::uint64_t value) noexcept {
    // Record that the work could not be done, and give a nonzero result.
    void Fail(std::uint64_t code, SvcFailure why) noexcept {
using SvcHandler = void (*)(SvcContext&);
class SyscallTable {
    // Room for the console's whole table with space to spare.
    static constexpr std::size_t kSlots = 1024;
    // Register a handler. Handlers are installed before any core runs, so the
    // table needs no lock for reads -- only the counters below are atomic,
    // because several cores dispatch at once.
    void Set(Svc id, SvcHandler handler);
    void Set(std::uint32_t swi, SvcHandler handler);
    bool Has(std::uint32_t swi) const noexcept;
    // Run the handler for `context.swi`, or record it as unimplemented.
    void Dispatch(SvcContext& context) noexcept;
    SvcFailure last_failure() const noexcept {
```

---

# `sync.h`

`内核/service/include/zlong/service/sync.h`

```
烛龙 (ZhuLong) - the waitable kernel objects.

Every one of them obeys the same three rules. They are what make waiting safe
rather than mostly-safe, and each rule exists because of a specific way the
obvious version goes wrong:

1. PrepareWait / PrepareLock REGISTERS the waiter and then reports whether
the caller must park at all. Registering first is what makes a signal
arriving in between impossible to lose. Parking first and registering
after drops that signal on the floor and strands the waiter forever.

2. A signal does NOT wake anybody. Signal / Release return the waiters that
were released, and the caller unparks them *after* dropping this object's
lock. That keeps the global lock order object.mutex -> core.mutex and
means no unpark happens while an object lock is held.

3. Every wait is a retry loop: a woken waiter re-runs the operation it was
doing from the top. Nothing here hands over ownership on wake, so a wait
that is cancelled can never leave its waiter holding something it does
not know about.

The objects do not know what a core is. A waiter is an opaque id; the layer
above maps it to a cpu::Core and does the parking.
```

```cpp
// Who to wake. Opaque here on purpose: this layer never parks anything.
using WaiterId = std::uint64_t;
// A manual/auto event.
class KEvent final : public KObject {
    static constexpr ObjectKind kKind = ObjectKind::Event;
    enum class Mode : std::uint8_t {
    // Register `waiter`. Returns true when it must park. False means the event
    // is already signalled -- and for an auto event this call consumed that
    // signal -- so the caller must proceed, not park.
    bool PrepareWait(WaiterId waiter);
    // Drop a registration. False when it was not registered.
    bool CancelWait(WaiterId waiter);
    // Clear a manual event's signal. An auto event clears itself when consumed.
    void Clear();
    bool signalled() const;
    Mode mode() const noexcept;
// A recursive mutex with an owner.
class KMutex final : public KObject {
    static constexpr ObjectKind kKind = ObjectKind::Mutex;
    // Take the lock. True when `owner` holds it now and must not park; false
    // when it was registered as a waiter. Re-entrant: an owner that already
    // holds it just deepens the count.
    bool PrepareLock(WaiterId owner);
    struct ReleaseResult {
    ReleaseResult Release(WaiterId owner);
    bool held() const;
    bool owned_by(WaiterId owner) const;
// A counting semaphore.
class KSemaphore final : public KObject {
    static constexpr ObjectKind kKind = ObjectKind::Semaphore;
    // Take a count. True when `waiter` must park; false when it took one.
    bool PrepareWait(WaiterId waiter);
    bool CancelWait(WaiterId waiter);
    struct SignalResult {
    SignalResult Signal(std::int32_t count);
```

---

# `thread.h`

`内核/service/include/zlong/service/thread.h`

```
烛龙 (ZhuLong) - a guest thread.

A thread is the unit that blocks. Its WaiterId IS its tid, so a woken waiter
is resolved by looking the tid up -- no separate registry of waiter ids, and
no way for the two to disagree.

The state machine is owned by the kernel: this type records a transition and
reports it, and never decides one.
```

```cpp
class Core;
class KThread final : public KObject {
    static constexpr ObjectKind kKind = ObjectKind::Thread;
    enum class State : std::uint8_t {
    // What a waitable object knows this thread by.
    WaiterId waiter_id() const noexcept { return tid_; }
    State state() const;
    void set_state(State state);
    // Where the thread begins, and what it begins with. Recorded at creation
    // and consumed when the core is started -- there is no context switching
    // yet, so a thread cannot start itself.
    void set_start(std::uint64_t entry, std::uint64_t stack, std::uint64_t argument);
    // The core this thread is bound to, or nullptr. Bound by the kernel when
    // the thread starts.
    //
    // A pointer rather than an index on purpose: waking a waiter has to reach
    // the core, and holding it directly means that path never needs a Cpu& to
    // look one up.
    cpu::Core* core() const;
    void set_core(cpu::Core* core);
    bool has_core() const;
const char* ToString(KThread::State state) noexcept;
```

---
