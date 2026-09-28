#include "zlong/service/svc.h"

namespace zlong::service {

void SyscallTable::Set(Svc id, SvcHandler handler) { Set(SvcNumber(id), handler); }

void SyscallTable::Set(std::uint32_t swi, SvcHandler handler) {
    if (swi >= kSlots) {
        return;  // out of range: not installed, and Has() will say so
    }
    if (handlers_[swi] == nullptr && handler != nullptr) {
        installed_.fetch_add(1, std::memory_order_relaxed);
    } else if (handlers_[swi] != nullptr && handler == nullptr) {
        installed_.fetch_sub(1, std::memory_order_relaxed);
    }
    handlers_[swi] = handler;
}

bool SyscallTable::Has(std::uint32_t swi) const noexcept {
    return swi < kSlots && handlers_[swi] != nullptr;
}

void SyscallTable::Dispatch(SvcContext& context) noexcept {
    const std::uint32_t swi = context.swi;
    if (swi >= kSlots || handlers_[swi] == nullptr) {
        // Unimplemented is not success. Say so, and leave a trace.
        context.Fail(kResultNotImplemented, "no handler for this syscall");
        last_unimplemented_.store(swi, std::memory_order_relaxed);
        unimplemented_calls_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    handlers_[swi](context);
    if (context.failure != nullptr) {
        failed_calls_.fetch_add(1, std::memory_order_relaxed);
        last_failure_.store(context.failure, std::memory_order_relaxed);
    }
}

}  // namespace zlong::service
