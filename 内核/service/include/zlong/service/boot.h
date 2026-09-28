// 烛龙 (ZhuLong) - bringing up the first process.
//
// Boot is the one place that knows the order: a process, the address space it
// runs in, the module loaded into it, and the main thread that starts at its
// entry point. Everything it builds already exists -- this is the sequence, not
// a new mechanism.
//
// It deliberately does NOT start a core. A host decides when cores run, and a
// test drives one synchronously from the entry point. See Kernel::ReadyThread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zlong/mount/environment.h"
#include "zlong/service/kernel.h"
#include "zlong/service/process.h"
#include "zlong/service/thread.h"

namespace zlong::cpu {
class Core;
}

namespace zlong::service {

/// A module to run: the bytes, where they go, and where execution starts.
struct ModuleImage {
    /// Guest virtual address to load at. Must be page aligned.
    std::uint64_t load_address = 0;
    std::vector<std::uint8_t> bytes;
    /// Where execution begins, as an offset from `load_address`. Must land
    /// inside the image.
    std::uint64_t entry_offset = 0;

    std::uint64_t entry() const noexcept { return load_address + entry_offset; }
};

struct BootConfig {
    std::uint64_t pid = 1;
    /// Stack size in bytes. Rounded up to a page.
    std::uint64_t stack_size = 0x1'0000;
    /// What the main thread's first argument register holds.
    std::uint64_t argument = 0;
};

/// A process that has been built, with a main thread ready to run.
///
/// The caller owns both, and has to keep them alive for as long as the kernel
/// may reach them: the kernel's thread registry only borrows (see
/// Kernel::BindThread). Unbind before letting either go.
struct BootedProcess {
    std::unique_ptr<KProcess> process;
    std::unique_ptr<KThread> main_thread;
    std::uint64_t entry = 0;
    std::uint64_t stack_top = 0;
};

/// Build the first process: enable the address space, load the module, map a
/// stack above it, create the process and its main thread, bind the thread to
/// `core`, and ready it at the entry point.
///
/// `error` receives the specific reason on failure, the same shape as
/// mount::LoadEnvironment.
std::optional<BootedProcess> Boot(Kernel& kernel, cpu::Core& core,
                                  const mount::SystemEnvironment& environment,
                                  const ModuleImage& image, const BootConfig& config,
                                  std::string& error);

}  // namespace zlong::service
