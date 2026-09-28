// 烛龙 (ZhuLong) - SVC dispatch.
//
// The guest reaches the kernel through one door: an SVC. This is the table
// behind it and the context a handler works with.
//
// Two rules, both about not lying to the guest:
//
//   1. An SVC with no handler returns a NONZERO result and is counted. Returning
//      zero -- the success code -- would tell the guest a call it never made
//      succeeded, and the failure would surface somewhere unrelated much later.
//
//   2. A handler that cannot do the work says so: it calls Fail(), which sets a
//      nonzero result and a reason. The dispatcher never converts a failure into
//      a success, and never guesses a "reasonable" return value.
//
// ============================================================================
// SVC NUMBERS: the enum below is THIS PROJECT'S PLACEHOLDER numbering, not the
// console's. The real numbers must be transcribed from the console's syscall
// list; until then they are deliberately parked in a block of our own so they
// cannot be mistaken for real ones. They live here so that replacing them is one
// edit, and nothing else in the tree spells a syscall number out.
// ============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "zlong/cpu/core.h"
#include "zlong/cpu/types.h"

namespace zlong::service {

/// The dispatcher. Forward-declared so a context can name it without this
/// header pulling the kernel in.
class Kernel;

/// Returned in x0 when an SVC has no handler. Nonzero and distinct on purpose:
/// "not implemented" must not be mistakable for success.
inline constexpr std::uint64_t kResultNotImplemented = 0x5A5A'0001ull;

/// The syscalls this layer knows about.
enum class Svc : std::uint32_t {
    // --- placeholders, in a block of our own (see the note above) -----------
    GetSystemTick = 0x100,
    OutputDebugString = 0x101,
    Break = 0x102,
    CreateEvent = 0x103,
    CreateMutex = 0x104,
    CreateSemaphore = 0x105,
    SignalEvent = 0x106,
    WaitSynchronization = 0x107,
    CloseHandle = 0x108,
    GetThreadId = 0x109,
    CreateThread = 0x10A,
    StartThread = 0x10B,
    ExitThread = 0x10C,
    SleepThread = 0x10D,
    ConnectToNamedPort = 0x10E,
    SendSyncRequest = 0x10F,
    SetHeapSize = 0x110,
    MapMemory = 0x111,
    UnmapMemory = 0x112,
    SetMemoryAttribute = 0x113,
};

constexpr std::uint32_t SvcNumber(Svc id) noexcept { return static_cast<std::uint32_t>(id); }

/// Event creation flags. PLACEHOLDERS, like the syscall numbers above: the
/// console's encoding has to be transcribed. Kept next to them so replacing
/// them is one edit.
inline constexpr std::uint64_t kEventManual = 1ull << 0;
inline constexpr std::uint64_t kEventSignalled = 1ull << 1;

/// A failure reason. A literal, not a string: a syscall must not allocate.
using SvcFailure = const char*;

struct SvcContext {
    cpu::Core& core;
    /// The layer that owns the thread registry, the handle tables and the
    /// service set. Two of those are needed by most syscalls, and a handler is a
    /// free function with no `this`.
    Kernel& kernel;
    std::uint32_t swi = 0;
    std::uint64_t args[8] = {};
    /// Written to x0 when the handler returns.
    std::uint64_t result = 0;
    /// Set by Fail(). The dispatcher counts these and keeps the reason.
    SvcFailure failure = nullptr;

    /// Somewhere for a handler to leave text, e.g. a debug string. A fixed
    /// buffer rather than a std::string: the length comes from the guest, and a
    /// syscall must not allocate on a path the guest controls.
    char text[256] = {};
    std::uint8_t text_length = 0;

    void SetResult(std::uint64_t value) noexcept {
        result = value;
        failure = nullptr;
    }

    /// Record that the work could not be done, and give a nonzero result.
    void Fail(std::uint64_t code, SvcFailure why) noexcept {
        result = code;
        failure = why;
    }
};

using SvcHandler = void (*)(SvcContext&);

class SyscallTable {
public:
    /// Room for the console's whole table with space to spare.
    static constexpr std::size_t kSlots = 1024;

    /// Register a handler. Handlers are installed before any core runs, so the
    /// table needs no lock for reads -- only the counters below are atomic,
    /// because several cores dispatch at once.
    void Set(Svc id, SvcHandler handler);
    void Set(std::uint32_t swi, SvcHandler handler);

    bool Has(std::uint32_t swi) const noexcept;

    /// Run the handler for `context.swi`, or record it as unimplemented.
    void Dispatch(SvcContext& context) noexcept;

    std::size_t installed() const noexcept { return installed_.load(std::memory_order_relaxed); }
    /// Calls that reached no handler.
    std::size_t unimplemented_calls() const noexcept {
        return unimplemented_calls_.load(std::memory_order_relaxed);
    }
    /// Calls whose handler reported a failure.
    std::size_t failed_calls() const noexcept {
        return failed_calls_.load(std::memory_order_relaxed);
    }
    std::uint32_t last_unimplemented() const noexcept {
        return last_unimplemented_.load(std::memory_order_relaxed);
    }
    SvcFailure last_failure() const noexcept {
        return last_failure_.load(std::memory_order_relaxed);
    }

private:
    SvcHandler handlers_[kSlots] = {};
    std::atomic<std::size_t> installed_{0};
    std::atomic<std::size_t> unimplemented_calls_{0};
    std::atomic<std::size_t> failed_calls_{0};
    std::atomic<std::uint32_t> last_unimplemented_{0};
    std::atomic<SvcFailure> last_failure_{nullptr};
};

}  // namespace zlong::service
