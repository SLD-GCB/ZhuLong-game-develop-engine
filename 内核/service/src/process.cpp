#include "zlong/service/process.h"

namespace zlong::service {

KProcess::KProcess(std::uint64_t pid) noexcept : KObject(kKind), pid_(pid) {}

void KProcess::Terminate() noexcept {
    std::lock_guard<std::mutex> lock(terminate_mutex_);
    if (terminated_.load(std::memory_order_acquire)) {
        return;  // idempotent: the first call wins
    }
    terminated_.store(true, std::memory_order_release);
    // A dying process must not leak a reference through its own table.
    handles_.CloseAll();
}

}  // namespace zlong::service
