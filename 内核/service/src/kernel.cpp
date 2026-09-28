#include "zlong/service/kernel.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "zlong/service/services/audout_service.h"
#include "zlong/service/services/audren_service.h"
#include "zlong/service/services/debug_service.h"
#include "zlong/service/services/fsp_service.h"
#include "zlong/service/services/nvdrv_service.h"
#include "zlong/service/services/time_service.h"

#include "zlong/audio/sink.h"
#include "zlong/gpu/gpu.h"

// Core::GetCpu() only forward-declares Cpu; the counter lives on the full type.
#include "zlong/cpu/cpu.h"

namespace zlong::service {

namespace {

/// Resolve a handle through the bound process. Null when there is no process, or
/// the handle names nothing.
KObject* Resolve(Kernel& kernel, Handle handle) {
    KProcess* process = kernel.process();
    if (process == nullptr) {
        return nullptr;
    }
    return process->handles().Get(handle);
}

/// The first batch of syscalls: the ones that need nothing but the core itself,
/// plus the ones that need the thread registry.
///
/// Argument layouts are documented per handler. They follow the console's
/// conventions where those are known and are this project's choices where they
/// are not; either way they are stated here rather than inferred from call
/// sites.

void GetSystemTick(SvcContext& context) {
    // [0] unused. Returns the counter, in the unit CNTFRQ_EL0 reports.
    context.SetResult(context.core.GetCpu().GetCNTPCT());
}

void OutputDebugString(SvcContext& context) {
    // [0] = address, [1] = maximum bytes.
    const std::uint64_t address = context.args[0];
    const std::uint64_t length = context.args[1];

    // The pointer and the length are guest-supplied: this is a trust boundary.
    // Read byte by byte through GuestMemory, which reports failure instead of
    // throwing, and stop at the first byte that is not there rather than
    // pretending the string was shorter.
    const std::size_t room = sizeof(context.text) - 1;
    std::size_t written = 0;
    for (std::uint64_t index = 0; index < length && written < room; ++index) {
        const auto byte = context.core.Memory().TryRead8(address + index);
        if (!byte.has_value() || *byte == 0) {
            break;
        }
        context.text[written++] = static_cast<char>(*byte);
    }
    context.text[written] = '\0';
    context.text_length = static_cast<std::uint8_t>(written);
    context.SetResult(0);
}

void Break(SvcContext& context) {
    // [0] = reason. A guest expects svcBreak to raise a guest exception. There is
    // no exception model yet, so the honest answer is a failure carrying the
    // reason code -- not a success, which would say it had been handled.
    context.Fail(context.args[0] != 0 ? context.args[0] : kResultNotImplemented,
                 "svcBreak: guest exception entry is not implemented");
}

void GetThreadId(SvcContext& context) {
    // No arguments. Returns the calling thread's id.
    KThread* thread = context.kernel.ThreadOn(context.core);
    if (thread == nullptr) {
        context.Fail(kResultNotImplemented, "no thread is bound to this core");
        return;
    }
    context.SetResult(thread->tid());
}

void CloseHandle(SvcContext& context) {
    // [0] = handle.
    KProcess* process = context.kernel.process();
    if (process == nullptr) {
        context.Fail(kResultNotImplemented, "no process is bound");
        return;
    }
    if (!process->handles().Close(static_cast<Handle>(context.args[0]))) {
        context.Fail(kResultNotImplemented, "the handle is not open");
        return;
    }
    context.SetResult(0);
}

/// Create an object, hand the guest a handle for it, and fail cleanly if either
/// half does not work.
void Publish(SvcContext& context, KObject* object, std::uint64_t out_address) {
    KProcess* process = context.kernel.process();
    if (process == nullptr) {
        object->Release();
        context.Fail(kResultNotImplemented, "no process is bound");
        return;
    }
    const Handle handle = process->handles().Allocate(object);
    if (handle == kInvalidHandle) {
        object->Release();
        context.Fail(kResultNotImplemented, "the handle table is full");
        return;
    }
    // The table holds a reference now; the creator's is dropped either way.
    object->Release();

    if (!context.core.Memory().TryWrite32(out_address, handle)) {
        process->handles().Close(handle);
        context.Fail(kResultNotImplemented, "could not write the handle to guest memory");
        return;
    }
    context.SetResult(0);
}

void CreateEvent(SvcContext& context) {
    // [0] = address to receive the handle, [1] = flags.
    const std::uint64_t out = context.args[0];
    const std::uint64_t flags = context.args[1];
    const bool manual = (flags & kEventManual) != 0;
    const bool signalled = (flags & kEventSignalled) != 0;

    auto* event = new KEvent(manual ? KEvent::Mode::Manual : KEvent::Mode::Auto, signalled);
    Publish(context, event, out);
}

void CreateMutex(SvcContext& context) {
    // [0] = address to receive the handle. Created unowned.
    Publish(context, new KMutex(), context.args[0]);
}

void CreateSemaphore(SvcContext& context) {
    // [0] = address to receive the handle, [1] = initial count, [2] = maximum.
    const std::uint64_t out = context.args[0];
    const auto initial = static_cast<std::int32_t>(context.args[1]);
    const auto maximum = static_cast<std::int32_t>(context.args[2]);
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        context.Fail(kResultNotImplemented, "the semaphore limits are not usable");
        return;
    }
    Publish(context, new KSemaphore(initial, maximum), out);
}

void SignalEvent(SvcContext& context) {
    // [0] = handle.
    if (!context.kernel.SignalEventHandle(static_cast<Handle>(context.args[0]))) {
        context.Fail(kResultNotImplemented, "that handle is not an event");
        return;
    }
    context.SetResult(0);
}

void WaitSynchronization(SvcContext& context) {
    // [0] = address of an array of handles, [1] = count, [2] = timeout in ns.
    const std::uint64_t addresses = context.args[0];
    const std::uint64_t count = context.args[1];
    const std::uint64_t timeout = context.args[2];

    // A nonzero timeout is refused rather than ignored: ignoring it would leave
    // a guest that asked to give up waiting waiting forever.
    if (timeout != 0) {
        context.Fail(kResultNotImplemented, "timed waits are not implemented");
        return;
    }
    if (count != 1) {
        context.Fail(kResultNotImplemented, "waiting on more than one handle is not implemented");
        return;
    }

    const auto raw = context.core.Memory().TryRead32(addresses);
    if (!raw.has_value()) {
        context.Fail(kResultNotImplemented, "could not read the handle from guest memory");
        return;
    }
    KObject* object = Resolve(context.kernel, static_cast<Handle>(*raw));
    if (object == nullptr) {
        context.Fail(kResultNotImplemented, "the handle names nothing");
        return;
    }

    KThread* thread = context.kernel.ThreadOn(context.core);
    if (thread == nullptr) {
        context.Fail(kResultNotImplemented, "no thread is bound to this core");
        return;
    }
    const WaiterId waiter = thread->waiter_id();

    // The retry loop. A wake hands nothing over: the object is left free and the
    // signal level-triggered, so this has to run again from the top -- the
    // object may well have gone to another waiter in between.
    for (;;) {
        bool must_park = true;
        switch (object->kind()) {
        case ObjectKind::Event:
            must_park = static_cast<KEvent*>(object)->PrepareWait(waiter);
            break;
        case ObjectKind::Semaphore:
            must_park = static_cast<KSemaphore*>(object)->PrepareWait(waiter);
            break;
        case ObjectKind::Mutex:
            must_park = !static_cast<KMutex*>(object)->PrepareLock(waiter);
            break;
        default:
            context.Fail(kResultNotImplemented, "that object is not waitable");
            return;
        }
        if (!must_park) {
            break;
        }

        thread->set_state(KThread::State::Waiting);
        context.core.ParkCurrentThread();  // legal: OnSvc runs outside Run()
        thread->set_state(KThread::State::Ready);
    }

    // The index of the handle that was signalled. With one handle, it is zero.
    context.SetResult(0);
}

void CreateThread(SvcContext& context) {
    // [0] = out pointer, [1] = entry, [2] = argument, [3] = stack top,
    // [4] = priority (not used yet), [5] = core id.
    const std::uint64_t out = context.args[0];
    const std::uint64_t entry = context.args[1];
    const std::uint64_t argument = context.args[2];
    const std::uint64_t stack = context.args[3];
    const std::uint64_t core_id = context.args[5];

    KProcess* process = context.kernel.process();
    if (process == nullptr) {
        context.Fail(kResultNotImplemented, "no process is bound");
        return;
    }
    cpu::Cpu& cpu = context.core.GetCpu();
    if (core_id >= cpu::Cpu::CoreCount()) {
        context.Fail(kResultNotImplemented, "that core does not exist");
        return;
    }
    cpu::Core& target = cpu.GetCore(static_cast<std::size_t>(core_id));

    // ONE guest thread per core, for now. A Core is one JIT with no saved
    // context, so a second thread on a busy core could not run at all -- and
    // accepting it would look like success while the thread never executed.
    if (context.kernel.ThreadOn(target) != nullptr) {
        context.Fail(kResultNotImplemented, "that core already runs a thread");
        return;
    }
    if (entry == 0 || stack == 0) {
        context.Fail(kResultNotImplemented, "the entry point or stack is not set");
        return;
    }

    auto* thread = new KThread(context.kernel.AllocateTid(), process->pid());
    thread->set_start(entry, stack, argument);
    if (!context.kernel.BindThread(*thread, target)) {
        thread->Release();
        context.Fail(kResultNotImplemented, "could not bind the thread to that core");
        return;
    }
    Publish(context, thread, out);
}

void StartThread(SvcContext& context) {
    // [0] = handle.
    if (!context.kernel.StartThreadHandle(static_cast<Handle>(context.args[0]))) {
        context.Fail(kResultNotImplemented, "that handle is not a thread that can start");
        return;
    }
    context.SetResult(0);
}

void ExitThread(SvcContext& context) {
    // No arguments.
    if (!context.kernel.ExitCurrentThread(context.core)) {
        context.Fail(kResultNotImplemented, "no thread is bound to this core");
        return;
    }
    context.SetResult(0);
}

void SleepThread(SvcContext& context) {
    // [0] = nanoseconds.
    if (context.args[0] != 0) {
        // There is no timer yet, so a real sleep cannot be honoured. Refusing is
        // the honest answer; sleeping for zero is what happens instead.
        context.Fail(kResultNotImplemented, "timed sleeps are not implemented");
        return;
    }
    context.SetResult(0);
}

// ---------------------------------------------------------------- IPC --------

void ConnectToNamedPort(SvcContext& context) {
    // [0] = out pointer, [1] = name address, [2] = name length.
    const std::uint64_t out = context.args[0];
    const std::uint64_t name_address = context.args[1];
    const std::uint64_t name_length = context.args[2];

    KProcess* process = context.kernel.process();
    if (process == nullptr) {
        context.Fail(kResultNotImplemented, "no process is bound");
        return;
    }
    if (name_length == 0 || name_length > kMaxServiceNameBytes) {
        context.Fail(kResultNotImplemented, "the port name length is out of range");
        return;
    }

    // A guest-supplied string: read it through the trust boundary.
    std::string name;
    name.reserve(static_cast<std::size_t>(name_length));
    for (std::uint64_t index = 0; index < name_length; ++index) {
        const auto byte = context.core.Memory().TryRead8(name_address + index);
        if (!byte.has_value()) {
            context.Fail(kResultNotImplemented, "the port name could not be read");
            return;
        }
        name.push_back(static_cast<char>(*byte));
    }

    IService* service = context.kernel.services().Find(name);
    if (service == nullptr) {
        context.Fail(kResultNotImplemented, "no service is registered under that name");
        return;
    }

    auto* session = new KSession(context.kernel.AllocateSessionId(), process->pid(), name);
    session->Attach(service);
    Publish(context, session, out);
}

void SendSyncRequest(SvcContext& context) {
    // [0] = session handle, [1] = command buffer address, [2] = room for the
    // response.
    //
    // NOTE: the console keeps the command buffer on the session rather than
    // passing it per call. That is part of the layout this project has not
    // transcribed, and it is stated here rather than left to be inferred.
    const std::uint64_t buffer = context.args[1];
    const std::uint64_t room = context.args[2];

    KObject* object = Resolve(context.kernel, static_cast<Handle>(context.args[0]));
    if (object == nullptr || object->kind() != ObjectKind::Session) {
        context.Fail(kResultNotImplemented, "that handle is not a session");
        return;
    }
    auto* session = static_cast<KSession*>(object);
    IService* service = session->service();
    if (service == nullptr) {
        context.Fail(kResultNotImplemented, "the session has no service attached");
        return;
    }

    KThread* thread = context.kernel.ThreadOn(context.core);
    if (thread == nullptr) {
        context.Fail(kResultNotImplemented, "no thread is bound to this core");
        return;
    }
    const WaiterId waiter = thread->waiter_id();

    // One request at a time per session. A wake hands nothing over, so this runs
    // again from the top: the session may have gone to another sender.
    for (;;) {
        if (session->BeginRequest(waiter)) {
            break;
        }
        thread->set_state(KThread::State::Waiting);
        context.core.ParkCurrentThread();
        thread->set_state(KThread::State::Ready);
    }

    IpcError ipc_error = IpcError::None;
    IpcRequest request;
    const bool unpacked = UnpackRequest(context.core.Memory(), buffer, request, &ipc_error);

    IpcResponse response;
    if (unpacked) {
        // Hand the service what it needs to answer: the caller's core, the
        // kernel it resolves through, and the connection this arrived on.
        ServiceContext service_context{context.core, context.kernel, *session};
        service->HandleRequest(service_context, request, response);
    } else {
        // The service never saw this request, and the guest is told so through
        // the response rather than by silence.
        response.result = kResultNotImplemented;
        response.data.clear();
    }

    const bool packed = PackResponse(context.core.Memory(), buffer, room, response, &ipc_error);

    // Release before waking: the wake must not run while the session lock is
    // held, which is what keeps the order session.mutex -> core.mutex.
    const std::vector<WaiterId> woken = session->EndRequest();
    context.kernel.WakeAll(woken);

    if (!unpacked || !packed) {
        context.Fail(kResultNotImplemented, "the command buffer could not be used");
        return;
    }
    context.SetResult(0);
}

// ------------------------------------------------------------- memory ------

void SetHeapSize(SvcContext& context) {
    // [0] = address to receive the heap base, [1] = requested size.
    const std::uint64_t out = context.args[0];
    const std::uint64_t size = context.args[1];

    KAddressSpace& space = context.kernel.address_space();
    if (!space.Enable()) {
        context.Fail(kResultNotImplemented, "the address space could not be enabled");
        return;
    }
    const std::uint64_t base = space.CreateHeap(size);
    if (base == 0) {
        context.Fail(kResultNotImplemented, "the heap could not be reserved");
        return;
    }
    if (!context.core.Memory().TryWrite64(out, base)) {
        context.Fail(kResultNotImplemented, "the heap base could not be written to guest memory");
        return;
    }
    context.SetResult(0);
}

void MapMemory(SvcContext& context) {
    // [0] = address, [1] = size, [2] = permissions.
    //
    // NOTE: the console maps memory the caller supplies; this maps a range of
    // the process own reserved space. That is one of the conventions this
    // project has not transcribed, stated here rather than left to be inferred.
    const std::uint64_t address = context.args[0];
    const std::uint64_t size = context.args[1];
    const auto permissions = static_cast<std::uint32_t>(context.args[2]);

    std::string why;
    if (!context.kernel.address_space().Map(address, size, permissions, why)) {
        context.Fail(kResultNotImplemented, why.c_str());
        return;
    }
    context.SetResult(0);
}

void UnmapMemory(SvcContext& context) {
    // [0] = address, [1] = size.
    const std::uint64_t address = context.args[0];
    const std::uint64_t size = context.args[1];

    std::string why;
    if (!context.kernel.address_space().Unmap(address, size, why)) {
        context.Fail(kResultNotImplemented, why.c_str());
        return;
    }
    context.SetResult(0);
}

void SetMemoryAttribute(SvcContext& context) {
    // [0] = address, [1] = size, [2] = mask, [3] = value. A set mask bit takes
    // the value bit; a clear one leaves that permission alone.
    const std::uint64_t address = context.args[0];
    const std::uint64_t size = context.args[1];
    const auto mask = static_cast<std::uint32_t>(context.args[2]) & (kMemoryRead | kMemoryWrite | kMemoryExecute);
    const auto value = static_cast<std::uint32_t>(context.args[3]) & (kMemoryRead | kMemoryWrite | kMemoryExecute);

    const KAddressSpace::Region* region = context.kernel.address_space().Find(address);
    if (region == nullptr) {
        context.Fail(kResultNotImplemented, "that address is not in a region");
        return;
    }
    const std::uint32_t updated = (region->permissions & ~mask) | (value & mask);

    std::string why;
    if (!context.kernel.address_space().SetAttributes(address, size, updated, why)) {
        context.Fail(kResultNotImplemented, why.c_str());
        return;
    }
    context.SetResult(0);
}

}  // namespace

Kernel::Kernel(ram::Ram& ram) : ram_(ram), address_space_(ram, ram.physical()) {
    syscalls_.Set(Svc::GetSystemTick, &GetSystemTick);
    syscalls_.Set(Svc::OutputDebugString, &OutputDebugString);
    syscalls_.Set(Svc::Break, &Break);
    syscalls_.Set(Svc::GetThreadId, &GetThreadId);
    syscalls_.Set(Svc::CloseHandle, &CloseHandle);
    syscalls_.Set(Svc::CreateEvent, &CreateEvent);
    syscalls_.Set(Svc::CreateMutex, &CreateMutex);
    syscalls_.Set(Svc::CreateSemaphore, &CreateSemaphore);
    syscalls_.Set(Svc::SignalEvent, &SignalEvent);
    syscalls_.Set(Svc::WaitSynchronization, &WaitSynchronization);
    syscalls_.Set(Svc::CreateThread, &CreateThread);
    syscalls_.Set(Svc::StartThread, &StartThread);
    syscalls_.Set(Svc::ExitThread, &ExitThread);
    syscalls_.Set(Svc::SleepThread, &SleepThread);
    syscalls_.Set(Svc::ConnectToNamedPort, &ConnectToNamedPort);
    syscalls_.Set(Svc::SendSyncRequest, &SendSyncRequest);

    // The first service. Registering it here means the IPC path always has one
    // thing to reach, so a break along it is visible immediately rather than at
    // the point a guest first happens to call it.
    syscalls_.Set(Svc::SetHeapSize, &SetHeapSize);
    syscalls_.Set(Svc::MapMemory, &MapMemory);
    syscalls_.Set(Svc::UnmapMemory, &UnmapMemory);
    syscalls_.Set(Svc::SetMemoryAttribute, &SetMemoryAttribute);

    services_.Register("debug", std::make_unique<DebugService>());

    // fsp-srv answers out of whatever the calling process has mounted, so it
    // needs nothing wired in: the mounts are the process's, and the process is
    // what a request arrives against.
    services_.Register("fsp-srv", std::make_unique<FspService>());

    // time reports intervals, not dates. See its header for why that is the
    // whole of it.
    services_.Register("time", std::make_unique<TimeService>());

    // nvdrv is registered whether or not a GPU is ever attached: the port the
    // guest connects to has to exist, and a submission with no GPU behind it is
    // refused with a reason rather than sent to nothing.
    auto nvdrv = std::make_unique<NvDrvService>();
    nvdrv_ = nvdrv.get();
    services_.Register("nvdrv", std::move(nvdrv));

    // audout is registered for the same reason, and its device is attached the same
    // way: the guest's samples go to the same sink the engine's own mixer writes to.
    auto audout = std::make_unique<AudOutService>();
    audout_ = audout.get();
    services_.Register("audout", std::move(audout));

    // audren is the same device seen from the other side. audout takes finished PCM and
    // passes it on; audren takes voices and produces the mix. Both hang off the one sink,
    // so a game gets out through the same road whichever of the two it drives.
    auto audren = std::make_unique<AudRenService>();
    audren_ = audren.get();
    services_.Register("audren", std::move(audren));
}

void Kernel::attach_gpu(gpu::Gpu& gpu) noexcept {
    nvdrv_->set_gpu(&gpu);
}

gpu::Gpu* Kernel::gpu() const noexcept {
    return nvdrv_->gpu();
}

void Kernel::attach_audio(audio::Sink& sink) noexcept {
    // One device, both services: whichever the guest drives, what it produces leaves by
    // the same road and reaches the same host program.
    audout_->set_sink(&sink);
    audren_->set_sink(&sink);
}

audio::Sink* Kernel::audio_sink() const noexcept {
    // Both services hold the same borrowed pointer, so either answers for the device.
    return audout_->sink();
}

// ------------------------------------------------------- the waiter bridge ---

bool Kernel::SignalEventHandle(Handle handle) {
    KObject* object = Resolve(*this, handle);
    if (object == nullptr || object->kind() != ObjectKind::Event) {
        return false;
    }
    // Signal returns the waiters and releases the object's lock before we touch
    // any core, which is what keeps the lock order object.mutex -> core.mutex.
    const std::vector<WaiterId> woken = static_cast<KEvent*>(object)->Signal();
    WakeAll(woken);
    return true;
}

bool Kernel::ReadyThread(KThread& thread) {
    if (thread.state() != KThread::State::Created) {
        return false;  // already started, or finished
    }
    cpu::Core* core = thread.core();
    if (core == nullptr) {
        return false;
    }

    // Give the guest its entry state. The core has not run, so this is safe --
    // there is no other thread to be caught mid-instruction.
    core->SetPC(thread.entry());
    core->SetSP(thread.stack());
    core->WriteGpr(0, thread.argument());
    thread.set_state(KThread::State::Ready);
    return true;
}

bool Kernel::StartThreadHandle(Handle handle) {
    KObject* object = Resolve(*this, handle);
    if (object == nullptr || object->kind() != ObjectKind::Thread) {
        return false;
    }
    auto* thread = static_cast<KThread*>(object);
    if (!ReadyThread(*thread)) {
        return false;
    }
    thread->core()->Start();
    return true;
}

bool Kernel::ExitCurrentThread(cpu::Core& core) {
    KThread* thread = ThreadOn(core);
    if (thread == nullptr) {
        return false;
    }
    thread->set_state(KThread::State::Terminated);
    // Unbind first: it is this layer that must stop naming the thread, and it
    // also clears the thread's core so a later wake cannot reach it.
    UnbindThread(thread->waiter_id());
    // RequestStop does not join, so this is safe from the core's own thread --
    // which is where a syscall handler runs.
    core.RequestStop();
    return true;
}

bool Kernel::BindThread(KThread& thread, cpu::Core& core) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const auto existing = threads_.find(thread.waiter_id());
    if (existing != threads_.end() && existing->second != &thread) {
        return false;
    }
    threads_[thread.waiter_id()] = &thread;
    thread.set_core(&core);
    return true;
}

bool Kernel::UnbindThread(WaiterId waiter) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const auto found = threads_.find(waiter);
    if (found == threads_.end()) {
        return false;
    }
    found->second->set_core(nullptr);
    threads_.erase(found);
    return true;
}

KThread* Kernel::FindThread(WaiterId waiter) const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const auto found = threads_.find(waiter);
    return found == threads_.end() ? nullptr : found->second;
}

KThread* Kernel::ThreadOn(cpu::Core& core) const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (const auto& entry : threads_) {
        if (entry.second->core() == &core) {
            return entry.second;
        }
    }
    return nullptr;
}

bool Kernel::Wake(WaiterId waiter) {
    KThread* thread = FindThread(waiter);
    if (thread == nullptr) {
        return false;
    }
    cpu::Core* core = thread->core();
    if (core == nullptr) {
        return false;
    }
    core->Unpark();  // the core's own latch covers an Unpark that beats the park
    return true;
}

std::size_t Kernel::WakeAll(const std::vector<WaiterId>& waiters) {
    std::size_t stranded = 0;
    for (const WaiterId waiter : waiters) {
        if (!Wake(waiter)) {
            ++stranded;
        }
    }
    if (stranded != 0) {
        stranded_wakes_.fetch_add(stranded, std::memory_order_relaxed);
    }
    return stranded;
}

// -------------------------------------------------------------- the door -----

void Kernel::OnSvc(cpu::Core& core, const cpu::SvcCall& call) {
    SvcContext context{core, *this, call.swi, {}, 0, nullptr, {}, 0};
    for (std::size_t index = 0; index < 8; ++index) {
        context.args[index] = core.ReadGpr(index);
    }

    syscalls_.Dispatch(context);

    if (context.text_length != 0) {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        last_debug_output_.assign(context.text, context.text_length);
        debug_strings_.fetch_add(1, std::memory_order_relaxed);
    }
    if (context.failure != nullptr && call.swi == SvcNumber(Svc::Break)) {
        last_break_reason_.store(context.result, std::memory_order_relaxed);
        breaks_.fetch_add(1, std::memory_order_relaxed);
    }

    core.WriteGpr(0, context.result);
}

void Kernel::OnInterrupt(cpu::Core& core, cpu::Interrupt interrupt) {
    (void)core;
    interrupts_seen_.fetch_add(1, std::memory_order_relaxed);
    last_interrupt_.store(static_cast<std::uint32_t>(interrupt), std::memory_order_relaxed);
    // Guest exception entry -- ELR_EL1, SPSR_EL1, VBAR_EL1, PSTATE masking -- is
    // not implemented, so this interrupt does NOT reach the guest. The counter
    // above is how that stays visible instead of looking like it worked.
}

std::string Kernel::last_debug_output() const {
    std::lock_guard<std::mutex> lock(debug_mutex_);
    return last_debug_output_;
}

}  // namespace zlong::service
