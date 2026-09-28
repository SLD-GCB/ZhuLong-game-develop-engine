# 内核-gpu

# `maxwell3d.h`

`内核/gpu/include/zlong/gpu/engine/maxwell3d.h`

```
烛龙 (ZhuLong) - the 3D engine's register state machine.

Turns decoded command-stream methods into a backend-neutral DrawDesc, which is
the thing both render backends consume.

The method addresses below are OUR model of the engine's register map: they
are isolated here and in maxwell3d.cpp so filling in the real map later is a
one-file change. Same treatment as the GOB layout and the method header.
```

```cpp
class Maxwell3D {
    // Shaders are bound by slot; the command stream selects a slot. The caller
    // owns the modules and must keep them alive while they may be drawn with.
    void SetShader(std::size_t slot, const shader::Module* vertex,
                   const shader::Module* fragment);
    // Start this engine with the same shader bindings as `other`.
    //
    // The shader table is the host's library standing in for the guest's own
    // shader upload, which is not decoded yet. A channel the guest opens therefore
    // begins with whatever the host installed, while everything a stream sets --
    // the selected slot, the targets, the viewport -- stays that channel's.
    void CopyShadersFrom(const Maxwell3D& other);
    // The address space every address a stream names is resolved through. Borrowed:
    // there is one per device, shared by every channel and by nvmap, because that is
    // what the hardware has.
    //
    // A null one resolves nothing, so every stream is refused -- deliberately. An
    // engine that fell back to treating the value as a physical address would hide
    // exactly the fault this exists to report: a stream naming memory nobody gave the
    // GPU.
    void set_address_space(const GpuAddressSpace* space) noexcept { address_space_ = space; }
    // Feed one decoded method. Returns false with `error` set only when a method
    // whose arguments we need was malformed; unknown methods are counted and
    // skipped so larger streams degrade instead of failing.
    bool Dispatch(const Method& method, const std::uint32_t* arguments, std::string& error);
    // True when a draw has been assembled since the last TakeDraw().
    bool HasDraw() const noexcept { return pending_.has_value(); }
    void Reset();
    bool Expect(const Method& method, std::uint32_t minimum, std::string& error);
    // Decode an address argument and resolve it through the address space.
    // False with a reason when the address was never mapped.
    bool Address(const Method& method, const std::uint32_t* arguments, std::uint64_t& out,
                 std::string& error) const;
    struct ShaderSlot {
```

---

# `pushbuffer.h`

`内核/gpu/include/zlong/gpu/engine/pushbuffer.h`

```
烛龙 (ZhuLong) - the guest command stream ("pushbuffer") decoder.

A pushbuffer is a stream of 32-bit words. A *method* is a header word followed
by its arguments; the header packs an opcode, an argument count, a subchannel
and a register address within the engine's method space.

Treat the bit layout below as our model of the format: it is isolated here and
in pushbuffer.cpp so correcting it is a one-file change.
```

```cpp
enum class MethodOpcode : std::uint8_t {
struct Method {
// Decoded method arguments, followed by the parsed header.
using MethodHandler = std::function<bool(const Method&, const std::uint32_t*)>;
class PushBufferReader {
    // Walk a decoded word stream. Returns false (with `error` set) on a
    // malformed or truncated stream; `emit` returning false stops the walk
    // cleanly and reports success, which is how an engine signals "I have
    // everything I need".
    static bool Walk(std::span<const std::uint32_t> words, const MethodHandler& emit,
                     std::string& error);
    // Convenience: read the stream out of guest memory first.
    static bool WalkMemory(const std::uint8_t* bytes, std::uint64_t size,
                           const MethodHandler& emit, std::string& error);
```

---

# `fault.h`

`内核/gpu/include/zlong/gpu/fault.h`

```
烛龙 (ZhuLong) - GPU faults.
```

```cpp
enum class GpuFaultKind : std::uint8_t {
struct GpuFault {
```

---

# `gpu.h`

`内核/gpu/include/zlong/gpu/gpu.h`

```
烛龙 (ZhuLong) - the GPU layer.

Renders into the guest's framebuffer in guest memory: there is deliberately
no window, no swapchain and no presentation here. Display is a separate,
later concern.

The layer never implements cpu::CpuHost. The kernel owns the mapping from
guest syscalls to Gpu::SubmitPushbuffer, and owns the decision to park a core
or raise an interrupt.
```

```cpp
class VkContext;
// What one submitted command stream amounted to.
enum class SubmitStatus : std::uint8_t {
const char* ToString(SubmitStatus status) noexcept;
struct SubmitResult {
    bool ok() const noexcept { return status == SubmitStatus::Ok; }
class Gpu {
    // Bring up Vulkan and run the capability probe. Returns false (with
    // last_error() set) when no usable device exists; the caller may continue
    // without a GPU.
    bool Initialize(bool enable_validation = false);
    void Shutdown();
    bool ready() const noexcept {
    const std::string& last_error() const noexcept { return error_; }
    vulkan::VkContext* vk() noexcept { return vk_.get(); }
    ram::PhysicalMemory& physical() noexcept { return physical_; }
    GpuHost& host() noexcept { return host_; }
    // The guest memory view the engine decodes out of and every backend reads
    // through. One place, so a GPU write still reaches the code-write observer.
    GpuMemoryManager& memory() noexcept { return memory_; }
    const GpuMemoryManager& memory() const noexcept { return memory_; }
    // The address space every GPU address a stream names is resolved through. One
    // per device, which is what the hardware has: nvmap hands out addresses in it and
    // the command decoder looks them up in it.
    GpuAddressSpace& address_space() noexcept { return address_space_; }
    const GpuAddressSpace& address_space() const noexcept { return address_space_; }
    // The channel the host-side engine and the tests submit through. A guest
    // opens its own with CreateChannel; this one exists so a caller driving a
    // single stream does not have to name a channel at all.
    static constexpr ChannelId kEngineChannel = 0;
    // A fresh register file. The handle stays valid until DestroyChannel, and
    // kInvalidChannel is returned when there is nothing to hand out.
    ChannelId CreateChannel();
    void DestroyChannel(ChannelId handle);
    // Install the backend every draw is executed on. Borrowed, and may be null:
    // a stream then still decodes, and each draw is reported as NoBackend rather
    // than silently dropped.
    void set_render_backend(render::RenderBackend* backend) noexcept { backend_ = backend; }
    render::RenderBackend* render_backend() const noexcept { return backend_; }
    // Bind the shaders a stream selects by slot. Borrowed: the caller owns the
    // modules and has to keep them alive while a stream may draw with them.
    void SetShader(ChannelId channel, std::size_t slot, const shader::Module* vertex,
                   const shader::Module* fragment);
    // Decode and run one command stream out of guest physical memory.
    //
    // Each draw the stream assembles is consumed and executed before the walk
    // goes on. That is not a shortcut: the engine holds one pending draw, so a
    // stream with two of them would otherwise keep only the last.
    //
    // Faults are reported through GpuHost and also returned. The layer reports;
    // the kernel decides what the guest is told.
    SubmitResult SubmitPushbuffer(ChannelId channel, GuestPa address, std::uint64_t size);
    // Convenience for a single-stream caller: these mean the engine channel.
    void SetShader(std::size_t slot, const shader::Module* vertex, const shader::Module* fragment) {
    SubmitResult SubmitPushbuffer(GuestPa address, std::uint64_t size) {
    // What to run when a submission finishes. Called on the render worker.
    using Completion = std::function<void(const SubmitResult&)>;
    // Hand a stream to the render worker and return at once.
    //
    // This is the shape a guest driver needs. Rendering on the submitting core --
    // which is what SubmitPushbuffer does, and all the host engine ever needs --
    // spends a whole guest core drawing and leaves the guest no way to do anything
    // else; a driver submits and then waits on a syncpoint instead. `on_done` runs
    // on the worker after this submission's draws have been published, in
    // submission order, so a syncpoint advanced from it means what it says.
    void SubmitPushbufferAsync(ChannelId channel, GuestPa address, std::uint64_t size,
                               Completion on_done);
    // One GPFIFO entry: where a command buffer is, and how long it is.
    //
    // Hardware's entry is two words and the segment it names runs until the GPU meets
    // an end-of-segment marker. Ours carries the length as a third word instead --
    // stated rather than guessed at, and it keeps the command stream itself free of a
    // marker the console may not have.
    static constexpr std::size_t kGpfifoEntryWords = 3;
    static constexpr std::size_t kGpfifoEntryBytes =
        kGpfifoEntryWords * sizeof(std::uint32_t);
        kGpfifoEntryWords * sizeof(std::uint32_t);
    // Decode and run a GPFIFO: a range inside the channel's bound GPFIFO buffer,
    // interpreted as entries, each naming a command buffer.
    //
    // This is the layer a driver submits through. The entry addresses are GPU
    // addresses and every one is resolved -- a GPFIFO that names a command buffer the
    // device was never told about is a fault, exactly as it is inside a stream.
    // SubmitPushbuffer stays the way to execute one command stream directly, which is
    // what the host engine does.
    SubmitResult SubmitGpfifo(ChannelId channel, GuestPa gpfifo, std::uint64_t byte_offset,
                              std::uint64_t byte_size);
    // The asynchronous form, for the guest's own submission path.
    void SubmitGpfifoAsync(ChannelId channel, GuestPa gpfifo, std::uint64_t byte_offset,
                           std::uint64_t byte_size, Completion on_done);
    // Run everything queued and wait for the worker to go idle.
    void WaitForIdle();
    // Where the last submission's time went, in milliseconds. Decoding a stream and
    // running it are different costs with different fixes, so they are counted apart.
    struct Profile {
    const Profile& profile() const noexcept { return profile_; }
    // One channel's state: its own register file, its own pending draw.
    struct Channel {
    struct Job {
    // Run one queued job on the worker.
    SubmitResult Run(const Job& job);
    Channel* FindChannel(ChannelId handle);
    bool ExecuteDraw(const render::DrawDesc& draw, SubmitResult& result);
    void ReportFault(GpuFaultKind kind, GuestPa address, std::uint32_t method,
                     const std::string& detail);
    // Start the render worker if it is not running. Only an asynchronous caller
    // pays for the thread.
    void EnsureWorker();
    void RenderWorker();
```

---

# `gpu_host.h`

`内核/gpu/include/zlong/gpu/gpu_host.h`

```
烛龙 (ZhuLong) - the GPU layer's callback seam into the kernel.

Policy lives in the kernel: the GPU layer reports events and never decides
whether a guest core should be interrupted. (Same shape as cpu::CpuHost.)
```

```cpp
class GpuHost {
    virtual ~GpuHost() = default;
    // A syncpoint reached (or passed) the given value. The kernel decides what
    // that means: unblock a parked core (already done for parkers), or
    // AssertInterrupt(Irq) if the core is in WFI instead of parked.
    virtual void OnSyncpointSignal(SyncpointId /*id*/, std::uint32_t /*value*/) {}
    // A GPU-side fault. Reported for diagnosis; not necessarily fatal.
    virtual void OnGpuFault(const GpuFault& /*fault*/) {}
```

---

# `gpu_address_space.h`

`内核/gpu/include/zlong/gpu/memory/gpu_address_space.h`

```
烛龙 (ZhuLong) - the GPU's own address space.

The console is unified memory: there is no separate VRAM, so an allocation's
backing store is guest physical memory and a GPU address is a way of NAMING that
memory, not a second copy of it. What this class needs to be, then, is a mapping
from GPU address to guest physical address and nothing more.

It does have to be a real mapping, though. Everything the guest submits names
memory this way, and a command stream that names an address nobody mapped is a
fault -- so the resolve is a lookup, not arithmetic at each point of use. A page
table would go in exactly here, and nothing outside this class can tell which one
is behind it.
```

```cpp
class GpuAddressSpace {
    // Where GPU addresses begin. Far above any guest physical layout, so a GPU
    // address and a physical address cannot be confused -- and if one is ever passed
    // where the other belongs, the resolve misses instead of quietly working.
    static constexpr GpuVAddr kBase = 0x0000'0100'0000'0000ull;
    // Name a physical range for the GPU. Returns the address the GPU uses for it.
    // Mapping the same range twice returns the address it already has.
    GpuVAddr Map(GuestPa physical, std::uint64_t size);
    // Forget the mapping that starts at `address`. True when one was there.
    bool Unmap(GpuVAddr address);
    void Clear();
    struct Mapping {
```

---

# `gpu_memory.h`

`内核/gpu/include/zlong/gpu/memory/gpu_memory.h`

```
烛龙 (ZhuLong) - guest RAM access for the GPU side.

This is the one place that routes guest memory writes made by the GPU.
PhysicalMemory::write deliberately does not notify the code-write observer
(only Ram::write does), so a GPU write that lands in executable guest memory
would otherwise leave stale JIT translations behind.
```

```cpp
class GpuMemoryManager : public MemorySource {
    explicit GpuMemoryManager(ram::PhysicalMemory& physical);
    void set_code_observer(cpu::CodeWriteObserver* observer) noexcept { observer_ = observer; }
    cpu::CodeWriteObserver* code_observer() const noexcept { return observer_; }
    // Ranges occupied by GPU page tables. A GPU write into such a range can
    // change translations anywhere, so it flushes every JIT cache. Defaults to
    // "no page tables", which the GPU address space registers later.
    using PageTablePredicate = std::function<bool(GuestPa pa, std::size_t n)>;
    void set_page_table_predicate(PageTablePredicate predicate) { page_table_ = std::move(predicate); }
    ram::PhysicalMemory& physical() noexcept { return physical_; }
    const ram::PhysicalMemory& physical() const noexcept { return physical_; }
    // MemorySource: read-only view for the shader interpreter and the software
    // renderer.
    const std::uint8_t* peek(GuestPa pa, std::uint64_t size) const override {
    bool GuestRead(GuestPa pa, void* dst, std::size_t n) const;
    // Write into guest RAM, then report the write to the code-write observer
    // when it matters (executable memory, or page-table memory).
    bool GuestWrite(GuestPa pa, const void* src, std::size_t n);
```

---

# `backend.h`

`内核/gpu/include/zlong/gpu/render/backend.h`

```
烛龙 (ZhuLong) - the render backend interface.

Two real implementations exist: a Vulkan backend and a CPU (software)
rasterizer. Neither is a stub: on a machine with no usable GPU the software
backend is what actually renders.
```

```cpp
struct BackendCaps {
enum class RenderStatus : std::uint8_t {
struct RenderResult {
    bool ok() const noexcept { return status == RenderStatus::Ok; }
    static RenderResult Good() { return {}; }
    static RenderResult Fail(RenderStatus status, std::string detail) {
class RenderBackend {
    virtual ~RenderBackend() = default;
    // "vulkan" or "software".
    virtual const char* name() const noexcept = 0;
    virtual BackendCaps caps() const noexcept = 0;
    // Returns false and fills `error`; never throws.
    virtual bool Initialize(std::string& error) = 0;
    virtual void Shutdown() = 0;
    virtual bool ready() const noexcept = 0;
    // Record one draw. Called only on the render worker thread, and runs to
    // completion.
    //
    // The pixels are NOT guaranteed to be in guest memory when this returns: a
    // backend is free to keep a run of draws to the same target resident and
    // publish them together, which is what stops a 65-draw frame from
    // round-tripping its target 65 times. Flush is what makes the result visible.
    virtual RenderResult Execute(const DrawDesc& draw) = 0;
    // Publish prior work into guest RAM and block until it is there. A caller that
    // reads a target it just drew into must call this first. The software backend
    // publishes as it goes, so this is a no-op there.
    virtual bool Flush(std::string& error) = 0;
    // Pure capability check, so the engine can report "this state is not
    // supported" instead of silently mis-rendering.
    virtual bool Supports(const DrawDesc& draw, std::string& why) const = 0;
```

---

# `draw.h`

`内核/gpu/include/zlong/gpu/render/draw.h`

```
烛龙 (ZhuLong) - the backend-neutral draw description.

This is THE contract between the command/state layer and every render backend.
It contains only guest physical addresses, our own enums, surface:: types and
shader::Module pointers -- no graphics API types, no handles, no ownership. A
backend resolves the addresses itself (the Vulkan backend is the only place a
binding becomes a VkBuffer).
```

```cpp
struct VertexAttribute {
struct RenderTarget {
struct ClearState {
struct DrawDesc {
```

---

# `mode.h`

`内核/gpu/include/zlong/gpu/render/mode.h`

```
烛龙 (ZhuLong) - render mode selection.

Some machines have only a CPU, so the choice between the GPU and CPU render
paths is explicit, observable, and never a silent fallback.
```

```cpp
enum class RenderMode : std::uint8_t {
enum class ActiveBackend : std::uint8_t { None, Vulkan, Software };
const char* ToString(RenderMode mode) noexcept;
const char* ToString(ActiveBackend backend) noexcept;
// Neutral capability description.
//
// Deliberately does NOT reference vulkan::Probe: the CPU-only build excludes
// the Vulkan sources entirely and must not be forced to include Vulkan headers
// just to describe what it has.
struct DeviceCapabilities {
struct ModeDecision {
    bool ok() const noexcept { return chosen != ActiveBackend::None; }
// Pure policy: performs no graphics API calls, so it is unit-testable with a
// synthetic capability description.
//
// Rules, in order:
// * requested Software -> Software.
// * requested Vulkan without a usable device -> NOT Ok (chosen == None), with
// vulkan_error set. A forced mode never silently degrades.
// * Auto -> Vulkan when the device has dynamic rendering + synchronization2,
// otherwise Software, naming the cause.
ModeDecision ChooseMode(RenderMode requested, const DeviceCapabilities& capabilities,
                        std::string vulkan_error);
```

---

# `state.h`

`内核/gpu/include/zlong/gpu/render/state.h`

```
烛龙 (ZhuLong) - backend-neutral pipeline state enums.
```

```cpp
struct Rect {
struct Viewport {
enum class PrimitiveTopology : std::uint8_t { Points, Lines, Triangles, TriangleStrip };
enum class IndexType : std::uint8_t { None, UInt16, UInt32 };
enum class CompareOp : std::uint8_t {
enum class CullMode : std::uint8_t { None, Front, Back };
enum class FrontFace : std::uint8_t { Clockwise, CounterClockwise };
enum class BlendFactor : std::uint8_t {
enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
struct RasterState {
struct DepthState {
struct BlendState {
```

---

# `resource.h`

`内核/gpu/include/zlong/gpu/resource.h`

```
烛龙 (ZhuLong) - references to guest memory as a resource.

Shared by the neutral draw description and the shader interpreter, so neither
has to duplicate the other's vocabulary. No graphics API types here.
```

```cpp
// A window onto guest memory.
struct BufferBinding {
struct TextureRef {
struct SamplerState {
    enum class Filter : std::uint8_t { Nearest, Linear };
    enum class Wrap : std::uint8_t { Repeat, Clamp, Mirror };
struct TextureBinding {
struct ConstantBufferBinding {
// Read-only view of guest memory. Lets the shader interpreter and the software
// renderer read resources without depending on the Vulkan side or on
// GpuMemoryManager's full interface.
class MemorySource {
    virtual ~MemorySource() = default;
    // Host pointer for a fully mapped range, or nullptr.
    virtual const std::uint8_t* peek(GuestPa pa, std::uint64_t size) const = 0;
```

---

# `interp.h`

`内核/gpu/include/zlong/gpu/shader/interp.h`

```
烛龙 (ZhuLong) - the shader IR interpreter.

This is the CPU render path's shader engine: the same IR the SPIR-V emitter
consumes is executed here directly, so a machine with no GPU runs real shaders
without needing a compiler.
```

```cpp
// Everything one shader invocation reads, and where it writes its results.
struct ShaderIo {
struct InterpResult {
    bool ok() const noexcept { return error.empty(); }
// Execute one invocation. Never throws.
InterpResult Interpret(const Module& module, const ShaderIo& in, ShaderIo& out);
```

---

# `ir.h`

`内核/gpu/include/zlong/gpu/shader/ir.h`

```
烛龙 (ZhuLong) - shader intermediate representation.

A register machine over a control-flow graph, not SSA: registers and
predicates map straight onto SPIR-V function-local variables, so no SSA
construction and no Phi are needed.

Two consumers share this IR:
* shader/interp   - interprets it (the CPU/software render path)
* shader/spirv_emitter - translates it (the Vulkan path)
```

```cpp
enum class Stage : std::uint8_t { Vertex, Fragment };
enum class RegKind : std::uint8_t { Gpr, Predicate };
enum class ScalarType : std::uint8_t { F32, U32, S32 };
// Comparison selector for the set-predicate ops.
enum class Compare : std::uint8_t {
enum class Op : std::uint16_t {
enum class Special : std::uint8_t {
// A register, a lane of one, or a predicate.
//
// A GPR index at or past `Function::gpr_count` is the zero register: it reads zero
// and a write to it goes nowhere. A shader's own encoding names one -- Maxwell's RZ
// is register 255 -- and both consumers read it the same way, so a decoded
// instruction can name it without either consumer having to know how large the
// other's register file turned out to be.
struct Value {
struct Instr {
struct Block {
struct Function {
// A shader input or output slot. Location numbering is backend-neutral, which is
// what lets one module serve both backends.
struct IoSlot {
struct Module {
```

---

# `maxwell.h`

`内核/gpu/include/zlong/gpu/shader/maxwell.h`

```
烛龙 (ZhuLong) - Maxwell shader binary: header, decoder, and cache.

The instruction encoding is real Maxwell SM50/SM52, transcribed from
envytools' envydis/gm107.c -- see shader/maxwell_encoding.h for the exact
source and for what is and is not represented. This file is the decoder: it
matches an instruction's (value, mask) opcode pattern, decodes the fields
that pattern carries, and lowers them onto the shared IR.

The binary *container* (magic, version, instruction count, register count) is
ours, not the hardware's: a real shader arrives as raw instruction bytes in
guest memory, and wiring that up is a separate step.

Two things the decoder deliberately does not do, and says so instead of
guessing: a register-indexed load/store/interpolation is rejected, and an
iadd3 whose third source is not RZ is rejected. fswzadd is accepted only in the
shape the IR can express -- a uniform swizzle over one source -- so a genuine
shuffle and a second source are rejected the same way. tex and tld are read
only as a two-dimensional, non-array, base-level fetch of a slot named by the
instruction's index; a descriptor in a register, another dimension, or any of
the flags the table names are rejected rather than dropped. fmnmx is read only
through the constant predicate -- a predicate register's value is not in the
stream -- and f2f only as a float-to-float conversion with floor rounding. An
operand's negate and absolute modifiers are lowered to `FNeg` and `FAbs`, since
neither consumer of the IR reads `Instr::negate` or `Instr::abs`, and the two
together on one operand are refused. RZ is resolved to a register the shader never
writes, which therefore reads zero in both consumers.
```

```cpp
// The shader binary's preamble (our model of it).
struct ShaderBinaryHeader {
inline constexpr std::size_t kShaderHeaderBytes = 16;
inline constexpr std::uint32_t kShaderMagic = 0x4853'4C5A;  // 'ZLSH' little-endian
inline constexpr std::uint32_t kShaderVersion = 1;
// Decode-once cache, keyed by the byte hash so a guest rewrite yields a new
// entry rather than needing invalidation.
class ShaderCache {
    // Returns nullptr and fills `error` when the binary cannot be decoded.
    const Module* Get(std::span<const std::uint8_t> bytes, Stage stage, std::string& error);
    void Clear() { modules_.clear(); }
```

---

# `maxwell_asm.h`

`内核/gpu/include/zlong/gpu/shader/maxwell_asm.h`

```
烛龙 (ZhuLong) - the Maxwell (SM50/SM52) instruction *encoder*.

maxwell.cpp reads a real instruction word and lowers it onto the IR; this is
the other direction -- a mnemonic and its operands, back to the word. The two
share one table (shader/maxwell_encoding.h) and the encoder takes each opcode
base out of it rather than repeating the constant, so a word written here is
one MatchOpcode lands on by construction.

The engine writes its shaders this way: assemble the words, hand them to
DecodeShader, and use the module that comes back. Round-tripping is what keeps
the two directions honest, and it is what the tests check.

Only the forms the decoder implements are encoded. There is no method for
anything the other direction would refuse, because writing it would produce
bytes that cannot come back; and a form nothing has a caller for is left out
rather than widened blind.
```

```cpp
// Which component of a source a broadcast repeats. The numbering is the
// table's: gm107.c tab5000_3 names selector value 0 "0000", so X is lane 0.
enum class Swizzle : std::uint32_t { X = 0, Y = 1, Z = 2, W = 3 };
// The two `mufu` functions the decoder lowers, named after the table's selectors.
enum class MufuFunction : std::uint32_t {
// A shader under construction, in Maxwell instruction words.
//
// Every method appends one 16-byte instruction and returns *this, so a program
// reads as the listing it is. Attribute operands are named by *slot*, the unit
// the IR and the renderer use; the byte offset the encoding actually carries is
// worked out here.
class MaxwellProgram {
    MaxwellProgram& Exit();
    MaxwellProgram& Mov32I(std::uint32_t dst, float value);
    MaxwellProgram& MovReg(std::uint32_t dst, std::uint32_t src);
    MaxwellProgram& MovConst(std::uint32_t dst, std::uint32_t bank, std::uint32_t byte_offset);
    MaxwellProgram& FAddReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FAddConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                              std::uint32_t byte_offset);
    // dst = a - b, as `fadd dst, a, -b`: Maxwell has no subtract, and the negate
    // modifier on the second source is what the decoder lowers back to a subtract.
    MaxwellProgram& FSubReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FMulReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FMulConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                              std::uint32_t byte_offset);
    MaxwellProgram& FFmaReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b, std::uint32_t c);
    MaxwellProgram& FFmaConst(std::uint32_t dst, std::uint32_t a, std::uint32_t bank,
                              std::uint32_t byte_offset, std::uint32_t c);
    MaxwellProgram& FFmaImm(std::uint32_t dst, std::uint32_t a, float b, std::uint32_t c);
    // fmnmx with the constant predicate both directions agree on. Which polarity
    // means minimum is not in the encoding table, so it is a convention rather
    // than a decoded fact -- see the encoding header and the decoder, which state
    // the same one. Only the register form is assembled, because only the
    // register form has a caller.
    MaxwellProgram& FMinReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    MaxwellProgram& FMaxReg(std::uint32_t dst, std::uint32_t a, std::uint32_t b);
    // dst = floor(src), assembled as f2f with floor rounding: this family has no
    // floor opcode of its own, and the conversion's mode field is what carries it.
    MaxwellProgram& Floor(std::uint32_t dst, std::uint32_t src);
    // dst = -src and dst = |src|, as `fadd dst, <modified src>, RZ`. A modifier is
    // what the hardware charges nothing for, and the encoder writes the two the
    // decoder lowers to `FNeg` and `FAbs`. Only the first source is modified: the
    // second is the zero register, so it contributes nothing but the addition.
    MaxwellProgram& Negate(std::uint32_t dst, std::uint32_t src);
    MaxwellProgram& Absolute(std::uint32_t dst, std::uint32_t src);
    MaxwellProgram& Mufu(std::uint32_t dst, std::uint32_t src, MufuFunction function);
    MaxwellProgram& LdConst(std::uint32_t dst, std::uint32_t bank, std::uint32_t byte_offset);
    MaxwellProgram& LdAttribute(std::uint32_t dst, std::uint32_t slot);
    MaxwellProgram& StoreAttribute(std::uint32_t slot, std::uint32_t src);
    MaxwellProgram& Ipa(std::uint32_t dst, std::uint32_t slot);
    // dst = texture `slot` sampled at the normalized coordinates in `coords`
    // (the same fetch the IR's SampleTexture is), and the same fetch at integer
    // coordinates with no filtering (the IR's LoadTexture). Both name a
    // two-dimensional, non-array texture at its base level, which is what the
    // decoder accepts: the slot rides in the instruction's index field and the
    // texture reference register is left as RZ.
    MaxwellProgram& Tex2D(std::uint32_t dst, std::uint32_t coords, std::uint32_t slot);
    MaxwellProgram& Tld2D(std::uint32_t dst, std::uint32_t coords, std::uint32_t slot);
    // dst = all four lanes of src set to one of src's components. This is the
    // IR's Splat, and in Maxwell it is `fswzadd` with a zero second source: the
    // only way the implemented set can move a value across lanes.
    MaxwellProgram& Broadcast(std::uint32_t dst, std::uint32_t src, Swizzle component);
    void Push(std::uint64_t word);
```

---

# `maxwell_encoding.h`

`内核/gpu/include/zlong/gpu/shader/maxwell_encoding.h`

```
烛龙 (ZhuLong) - the Maxwell (SM50/SM52) shader instruction encoding, as data.

============================================================================
PROVENANCE -- read before changing any value in this file.

Transcribed from envytools' machine-readable instruction table:
envytools/envydis/gm107.c          (fetched 2026-09-25)
That table is what envyas and envydis are generated from, and it is the
thing emulators transcribe. The prose ISA reference documents only a
subset of behaviour and says so:
https://envytools.readthedocs.io/en/latest/hw/graph/maxwell/cuda/isa.html
("This currently is not a complete reference ... where behaviour not
obvious from envydis/gm107.c can be documented.")

The Switch's GPU is GM20B, i.e. SM52. gm107_isa_s wires the "sm" variantset
(sm50 -> sm50op, sm52 -> sm50op+sm52op, sm60 -> sm52op+sm60op), so the
SM50/SM52 opcodes are the applicable ones.

Every bit position, opcode value and opcode mask below is quoted from that
file. Where envydis has a name for a field (U32_20, S20_2, C34_RZ_O14_20,
C36_08_S16_20, AMEM, ...) the envydis name is kept so the transcription can
be checked against its source. envydis scans a table linearly and takes the
first entry whose (value, mask) matches, so the order of kOpcodePatterns is
part of the data, not a style choice.

There is no single "opcode width": bit 56 doubles as the 20th bit of the
split short immediate, which is why mov's mask is 0xfef8... while mov32i's
is 0xfff0.... The mask carried by each pattern is therefore authoritative.

NOT represented here: the control/scheduling half of the instruction (the
second 64-bit word: tabsched's three 21-bit slots of stall/yield/barrier
fields), the operand size and mode tables (T(ef90sz), T(ef58sz),
T(eff0_0), T(a000_0), T(a000_1), T(fbe0_0), ...), the branch-target and
constant-bank-index registers, and the ~200 opcodes the decoder does not
implement.
============================================================================
```

```cpp
// How one 128-bit instruction word packs its fields.
struct InstructionFormat {
inline constexpr InstructionFormat kFormat{};
// The opcodes this decoder implements. Each is a real SM50/SM52 mnemonic; the
// comment carries the encoding it is matched by.
enum class ShaderOpcode : std::uint32_t {
// Which kind of second operand a pattern carries. It is the form that decides
// how the instruction is lowered, because the IR distinguishes a register
// source from a constant or immediate one.
enum class SourceKind : std::uint8_t {
struct OpcodePattern {
// Scanned in order; the first match wins, exactly as envydis scans tabroot.
inline constexpr OpcodePattern kOpcodePatterns[] = {
// The first pattern whose masked bits match, or nullptr. envydis semantics.
inline const OpcodePattern* MatchOpcode(std::uint64_t word) {
```

---

# `spirv_emitter.h`

`内核/gpu/include/zlong/gpu/shader/spirv_emitter.h`

```
烛龙 (ZhuLong) - IR to SPIR-V.

The second consumer of the shader IR (the first is shader/interp). Numbers are
emitted as function-local float variables, one per (register, lane), which
keeps everything straight-line and avoids SSA construction.

Scope today: load-attribute, load-immediate, move, read-special, store-output,
kill, return, and the float arithmetic set. Predicates, control flow, integer
and texture operations are reported as errors rather than mis-emitted.
```


---

# `backend.h`

`内核/gpu/include/zlong/gpu/software/backend.h`

```
烛龙 (ZhuLong) - the CPU render backend.

This is not a placeholder: on a machine with no usable GPU it is what actually
renders, and it is the always-available reference the Vulkan path is checked
against.
```

```cpp
class SoftwareBackend final : public render::RenderBackend {
    explicit SoftwareBackend(GpuMemoryManager& memory);
    const char* name() const noexcept override { return "software"; }
    render::BackendCaps caps() const noexcept override;
    bool Initialize(std::string& error) override;
    void Shutdown() override;
    bool ready() const noexcept override { return true; }
    render::RenderResult Execute(const render::DrawDesc& draw) override;
    // Always true: Execute is synchronous, so the pixels are already in guest
    // memory by the time it returns.
    bool Flush(std::string& error) override;
    bool Supports(const render::DrawDesc& draw, std::string& why) const override;
```

---

# `pool.h`

`内核/gpu/include/zlong/gpu/software/pool.h`

```
烛龙 (ZhuLong) - a worker pool for the software renderer.

The pool is owned by the backend and not by a draw: spawning threads per draw
would cost more than the draws themselves. `Run` makes the calling thread one of
the workers, so a machine with two cores is not left with one core idle while the
other waits.
```

```cpp
class ThreadPool {
    // `workers` background threads, so total parallelism is workers + 1.
    explicit ThreadPool(std::size_t workers);
    // Run body(task) for every task in [0, tasks), on this thread and the workers,
    // and return when all of them are done. Not reentrant.
    void Run(std::size_t tasks, const std::function<void(std::size_t)>& body);
    void Worker();
    // Valid only while a Run is in flight; guarded by `mutex_`.
    const std::function<void(std::size_t)>* body_ = nullptr;
```

---

# `rasterizer.h`

`内核/gpu/include/zlong/gpu/software/rasterizer.h`

```
烛龙 (ZhuLong) - the CPU rasterizer: vertex shading, triangle setup, the
fragment stage (shader + depth + blend), and writeback.

Deliberately simple and correct rather than fast: this is the path for
machines with no usable GPU, and the always-available reference for checking
the GPU path against.
```

```cpp
class Rasterizer {
    // Runs to completion and leaves the colour target in guest memory.
    render::RenderResult Draw();
```

---

# `target.h`

`内核/gpu/include/zlong/gpu/software/target.h`

```
烛龙 (ZhuLong) - a guest surface as a CPU-rendered target.

The surface is staged in host memory and written back in one go, so guest
writes go through GpuMemoryManager::GuestWrite exactly once per draw and code
invalidation is routed for the whole range.
```

```cpp
class SoftwareRenderTarget {
    bool valid() const noexcept { return valid_; }
    // Fill every texel with the clear value.
    void Clear(const render::ClearState& state);
    bool Load(std::uint32_t x, std::uint32_t y, std::array<float, 4>& rgba) const;
    bool Store(std::uint32_t x, std::uint32_t y, const std::array<float, 4>& rgba);
    // Publish the staged surface back to guest memory (routed, so executable
    // and page-table ranges are reported).
    bool Flush();
```

---

# `format.h`

`内核/gpu/include/zlong/gpu/surface/format.h`

```
烛龙 (ZhuLong) - guest surface formats, described neutrally.

Vulkan is deliberately absent here: this table is shared by the Vulkan
backend and the CPU (software) backend, and the CPU-only build must not need
the Vulkan SDK.
```

```cpp
enum class SurfaceFormat : std::uint16_t {
enum class ComponentKind : std::uint8_t {
// What a consumer must do beyond reading the raw bytes.
enum class FormatConversion : std::uint8_t {
struct FormatInfo {
FormatInfo DecodeFormat(SurfaceFormat format) noexcept;
// Usable as a colour render target.
bool IsColourTarget(SurfaceFormat format) noexcept;
// Usable as a depth/stencil target.
bool IsDepthTarget(SurfaceFormat format) noexcept;
```

---

# `layout.h`

`内核/gpu/include/zlong/gpu/surface/layout.h`

```
烛龙 (ZhuLong) - guest surface memory layouts.

The block-linear (GOB) addressing formula lives ONLY in layout.cpp. Treat it
as our model of the layout rather than verified hardware behaviour: the real
Tegra layout interleaves GOBs in groups, and correcting that is a change to
one file.
```

```cpp
enum class TileMode : std::uint8_t {
struct Surface {
// Byte offset of texel (x, y), or `kInvalidOffset` when out of range.
inline constexpr std::size_t kInvalidOffset = static_cast<std::size_t>(-1);
// Convert a tightly packed linear image (width * bpp per row) into the
// surface's layout.
void SwizzleIn(std::span<const std::uint8_t> linear, const Surface& surface,
               std::span<std::uint8_t> out);
// Convert the surface's layout into a tightly packed linear image.
void SwizzleOut(std::span<const std::uint8_t> stored, const Surface& surface,
                std::span<std::uint8_t> out);
```

---

# `texel.h`

`内核/gpu/include/zlong/gpu/surface/texel.h`

```
烛龙 (ZhuLong) - per-texel format conversion.

One place for turning a format's raw bytes into linear floating RGBA and back,
shared by the shader interpreter (texture reads) and the software renderer
(render-target reads/writes).
```

```cpp
// Encode linear floating RGBA into one texel.
void EncodeTexel(const FormatInfo& info, const std::array<float, 4>& rgba, std::uint8_t* bytes);
```

---

# `types.h`

`内核/gpu/include/zlong/gpu/types.h`

```
烛龙 (ZhuLong) - GPU layer common types.
```

```cpp
// A GPU virtual address, as submitted by the guest driver. Distinct from the
// CPU's virtual addresses: GPU addresses are translated by the GPU's own page
// tables (GpuAddressSpace), not by ram::Mmu.
using GpuVAddr = std::uint64_t;
// A guest physical address, i.e. an address in the space modelled by
// zlong::ram::PhysicalMemory.
using GuestPa = std::uint64_t;
using ChannelId = std::uint32_t;
using SyncpointId = std::uint32_t;
inline constexpr ChannelId kInvalidChannel = 0xFFFF'FFFFu;
inline constexpr SyncpointId kInvalidSyncpoint = 0xFFFF'FFFFu;
// Which engine a channel targets. Order matches the engine array index.
enum class ChannelType : std::uint8_t {
inline constexpr std::size_t kChannelTypeCount = 4;
```

---

# `vk_context.h`

`内核/gpu/include/zlong/gpu/vulkan/vk_context.h`

```
烛龙 (ZhuLong) - Vulkan instance/device/queue plus the first-run capability
probe.

Nothing in this layer assumes the host GPU can do anything: whether host
memory can be imported, and with what alignment, is established by the probe
at startup.
```

```cpp
struct DeviceInfo {
// Results of the capability probe. The memory plan depends on these, so they
// are measured rather than assumed.
struct Probe {
struct HostPointerProperties {
class VkContext {
    // Create instance + device + queue and run the probe. Returns nullptr and
    // fills `error` when there is no usable Vulkan device.
    static std::unique_ptr<VkContext> Create(bool enable_validation, std::string& error);
    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physical_device() const noexcept { return physical_; }
    VkDevice device() const noexcept { return device_; }
    VkQueue queue() const noexcept { return queue_; }
    const DeviceInfo& info() const noexcept { return info_; }
    const Probe& probe() const noexcept { return probe_; }
    const VkPhysicalDeviceProperties& properties() const noexcept { return properties_; }
    // Which memory types can import `host_pointer`.
    HostPointerProperties QueryHostPointer(const void* host_pointer) const;
    VkCommandPool CreateCommandPool(VkCommandPoolCreateFlags flags = 0) const;
    void Destroy();
    void CreateDebugMessenger();
    void DestroyDebugMessenger();
```

---

# `vk_format.h`

`内核/gpu/include/zlong/gpu/vulkan/vk_format.h`

```
烛龙 (ZhuLong) - the ONLY place a guest surface format meets VkFormat.
```

```cpp
VkFormat ToVkFormat(surface::SurfaceFormat format) noexcept;
```

---

# `vk_memory.h`

`内核/gpu/include/zlong/gpu/vulkan/vk_memory.h`

```
烛龙 (ZhuLong) - guest RAM as seen by Vulkan.

The Switch is a unified-memory machine: there is no separate VRAM, so "显存" is
a subset of guest physical memory. This class therefore tries to alias the
existing host allocation (VK_EXT_external_memory_host, one physical copy,
coherent with the CPU for free) and falls back to a device-local copy when the
host can only offer PCIe-visible memory.
```

```cpp
enum class GuestMemoryMode : std::uint8_t {
// A window onto a guest memory range.
struct GuestBuffer {
class VkMemory {
    struct Options {
    GuestMemoryMode mode() const noexcept { return mode_; }
    // Why this mode was chosen. Worth asserting on: on a discrete GPU the
    // importable memory may be host-visible only, which is not a fast path.
    const std::string& mode_reason() const noexcept { return mode_reason_; }
    void set_memory_manager(GpuMemoryManager* memory) noexcept { guest_ = memory; }
    void DestroyBuffer(GuestBuffer& buffer);
    // Make the device copy match guest RAM. No-op when imported.
    bool Upload(const GuestBuffer& buffer, GuestPa pa, std::uint64_t size);
    // Make guest RAM match the device copy (routed through
    // GpuMemoryManager::GuestWrite). No-op when imported.
    bool Download(const GuestBuffer& buffer, GuestPa pa, std::uint64_t size);
    // Fill the buffer's guest range with `value` on the GPU (vkCmdFillBuffer).
    // Offset and size must be multiples of 4.
    bool Fill(const GuestBuffer& buffer, std::uint32_t value);
    struct ImportedRegion {
    bool TryImportRegions();
    const ImportedRegion* FindRegion(GuestPa pa, std::uint64_t size) const;
    bool CreateStaging(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                       void** mapped) const;
    void DestroyStaging(VkBuffer buffer, VkDeviceMemory memory, void* mapped) const;
    VkCommandBuffer BeginOneShot() const;
    bool SubmitAndWait(VkCommandBuffer commands);
    void EndOneShot(VkCommandBuffer commands) const;
```

---

# `vk_renderer.h`

`内核/gpu/include/zlong/gpu/vulkan/vk_renderer.h`

```
烛龙 (ZhuLong) - the Vulkan render backend.

Renders into a device-local image and publishes the result into the guest
framebuffer. Colour targets, depth targets, constant buffers and up to two
sampled textures are wired up; anything else is reported as unsupported rather
than mis-rendered.
```

```cpp
class VulkanBackend final : public render::RenderBackend {
    const char* name() const noexcept override { return "vulkan"; }
    render::BackendCaps caps() const noexcept override;
    bool Initialize(std::string& error) override;
    void Shutdown() override;
    bool ready() const noexcept override { return ready_; }
    render::RenderResult Execute(const render::DrawDesc& draw) override;
    bool Flush(std::string& error) override;
    bool Supports(const render::DrawDesc& draw, std::string& why) const override;
    struct ModuleEntry {
    // A device-local depth image kept per guest depth surface, so depth persists
    // across draws the same way the software backend's staged surface does.
    struct DepthEntry {
    DepthEntry* AcquireDepth(const render::DrawDesc& draw, std::string& error);
    bool UploadDepth(DepthEntry& entry, GuestPa pa, std::uint64_t size, std::string& error);
    bool DownloadDepth(const DepthEntry& entry, GuestPa pa, std::uint64_t size);
    // A pass is a run of draws that share one colour target and one depth target --
    // the shadow pass, then the main pass. Both images stay in device memory for
    // the whole run and guest memory is touched only when the pass opens and
    // closes, which is what stops a 65-draw frame from round-tripping every target
    // 65 times.
    struct PassKey {
    // A texture uploaded for the current pass. Two drawables in one pass usually
    // share their textures -- and every main-pass drawable samples the same shadow
    // map -- so decoding one per draw is pure waste.
    struct CachedTexture {
    static PassKey KeyOf(const render::DrawDesc& draw);
    bool BeginPass(const render::DrawDesc& draw, std::string& error);
    bool EndPass(std::string& error);
    // Release the open pass without submitting it. Used when a draw in the middle
    // of a pass fails: the recorded commands would leave a hole in the result.
    void AbandonPass();
    render::RenderResult RecordDraw(const render::DrawDesc& draw);
    CachedTexture* AcquireTexture(const TextureBinding& binding, std::string& error);
    void DestroyTexture(CachedTexture& texture);
    // A device buffer standing in for a guest range. Guest geometry and constants
    // are uploaded once by the engine and then bound by every draw that uses them,
    // so creating a fresh buffer per draw was pure waste -- and in the imported
    // memory mode it is not needed at all.
    struct CachedBuffer {
    static constexpr std::size_t kNoBuffer = static_cast<std::size_t>(-1);
    // Record the guest bytes into the buffer, inside the pass command buffer. In
    // the imported mode there is nothing to record: the device reads guest RAM.
    bool RecordUpload(const CachedBuffer& cached, std::string& error);
    void ReleaseBuffers();
    // Seed a freshly created image from a tightly packed linear buffer. Needed
    // whenever a draw does not clear: the image is created per draw, so without
    // this the previous contents would be undefined.
    bool UploadImage(VkImage image, std::uint32_t width, std::uint32_t height,
                     VkImageAspectFlags aspect, VkImageLayout final_layout,
                     const std::uint8_t* bytes, std::uint64_t size);
    // Copy a guest surface into a tightly packed linear buffer, undoing whatever
    // layout and pitch the guest used.
    bool LoadLinearFromGuest(const render::RenderTarget& target, std::vector<std::uint8_t>& out);
    VkShaderModule GetShaderModule(const shader::Module& module, std::string& error);
    VkPipeline GetPipeline(const render::DrawDesc& draw, VkShaderModule vs, VkShaderModule fs,
                           VkFormat colour_format, VkFormat depth_format, std::string& error);
    bool CreateStagingBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                             void** mapped, std::string& error);
    void DestroyStagingBuffer(VkBuffer buffer, VkDeviceMemory memory);
    bool PublishToGuest(const render::RenderTarget& target, const std::uint8_t* pixels,
                        std::string& error);
    // Create a device-local sampled image from a guest texture, plus a view and
    // a sampler matching the requested filter and wrap modes.
    bool CreateTexture(const TextureBinding& binding, VkImage& image, VkDeviceMemory& memory,
                       VkImageView& view, VkSampler& sampler, std::string& error);
    // The same, but handing the guest bytes to the GPU instead of decoding them on
    // the host: valid only when the guest texture is linear and its format maps to a
    // Vulkan one, and only in the imported memory mode where the source needs no
    // copy of its own. `false` means "use CreateTexture instead".
    bool CreateTextureFromGuest(const TextureBinding& binding, CachedTexture& out,
                                std::string& error);
```

---
