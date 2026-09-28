#include "zlong/service/object.h"

namespace zlong::service {

const char* ToString(ObjectKind kind) noexcept {
    switch (kind) {
    case ObjectKind::Thread:
        return "thread";
    case ObjectKind::Process:
        return "process";
    case ObjectKind::Session:
        return "session";
    case ObjectKind::Event:
        return "event";
    case ObjectKind::Mutex:
        return "mutex";
    case ObjectKind::Semaphore:
        return "semaphore";
    }
    return "unknown";
}

void KObject::AddRef() noexcept { refs_.fetch_add(1, std::memory_order_acq_rel); }

bool KObject::Release() noexcept {
    // acq_rel on the decrement: the releasing thread's writes must be visible to
    // whoever ends up destroying the object.
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
        return true;
    }
    return false;
}

}  // namespace zlong::service
