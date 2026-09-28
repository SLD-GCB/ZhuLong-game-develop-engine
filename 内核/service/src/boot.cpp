#include "zlong/service/boot.h"

namespace zlong::service {

namespace {

constexpr std::uint64_t kPageMask = ram::Mmu::kPageMask;

std::uint64_t RoundUpToPage(std::uint64_t value) noexcept {
    return (value + kPageMask) & ~kPageMask;
}

}  // namespace

std::optional<BootedProcess> Boot(Kernel& kernel, cpu::Core& core,
                                  const mount::SystemEnvironment& environment,
                                  const ModuleImage& image, const BootConfig& config,
                                  std::string& error) {
    if (image.bytes.empty()) {
        error = "the module image is empty";
        return std::nullopt;
    }
    if ((image.load_address & kPageMask) != 0) {
        error = "the module load address is not page aligned";
        return std::nullopt;
    }
    if (image.entry_offset >= image.bytes.size()) {
        error = "the entry point is outside the module image";
        return std::nullopt;
    }
    if (config.stack_size == 0) {
        error = "the stack has no size";
        return std::nullopt;
    }

    KAddressSpace& space = kernel.address_space();
    if (!space.Enable()) {
        error = "the address space could not be enabled";
        return std::nullopt;
    }

    // Read and executed, never written. That is what makes a stray store to the
    // module a fault instead of a silent surprise.
    if (!space.LoadImage(image.load_address, image.bytes.data(), image.bytes.size(),
                         kMemoryRead | kMemoryExecute, error)) {
        return std::nullopt;
    }

    // The stack goes above the image, at the first address nothing covers, and
    // grows down from its top. Taking the address from the reservation table
    // rather than a constant is what keeps the two from overlapping.
    const std::uint64_t stack_size = RoundUpToPage(config.stack_size);
    const std::uint64_t stack_base = space.next_free();
    if (!space.Reserve(stack_base, stack_size, error)) {
        return std::nullopt;
    }
    if (!space.Map(stack_base, stack_size, kMemoryRead | kMemoryWrite, error)) {
        return std::nullopt;
    }
    const std::uint64_t stack_top = stack_base + stack_size;

    auto process = std::make_unique<KProcess>(config.pid);
    process->set_mounts(&environment.mounts);
    process->set_address_space(&space);

    auto main_thread = std::make_unique<KThread>(kernel.AllocateTid(), config.pid);
    main_thread->set_start(image.entry(), stack_top, config.argument);

    // From here on the kernel names these objects, and it only borrows them: a
    // failure has to take the registration back out before the unique_ptr frees
    // what the registry still points at. So the process is handed to the kernel
    // last, once nothing else can fail.
    if (!kernel.BindThread(*main_thread, core)) {
        error = "the main thread could not be bound to that core";
        return std::nullopt;
    }
    if (!kernel.ReadyThread(*main_thread)) {
        kernel.UnbindThread(main_thread->waiter_id());
        error = "the main thread could not be readied";
        return std::nullopt;
    }

    process->set_main_thread(main_thread.get());
    // Syscalls resolve handles through the bound process, so this has to name
    // the process that is about to run.
    kernel.set_process(process.get());

    BootedProcess booted;
    booted.entry = image.entry();
    booted.stack_top = stack_top;
    booted.process = std::move(process);
    booted.main_thread = std::move(main_thread);
    return booted;
}

}  // namespace zlong::service
