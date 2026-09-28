#include "zlong/service/handle.h"

namespace zlong::service {

namespace {

Handle Compose(std::size_t index, std::uint32_t generation) noexcept {
    return ((generation & kHandleIndexMask) << 16) |
           (static_cast<std::uint32_t>(index) + 1u);
}

}  // namespace

HandleTable::~HandleTable() { CloseAll(); }

void HandleTable::Retire(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    slot.object = nullptr;
    // Bump the generation so every handle that named the old object now misses.
    slot.generation = (slot.generation + 1u) & kHandleIndexMask;
    free_slots_.push_back(static_cast<std::uint32_t>(index) + 1u);
}

Handle HandleTable::Allocate(KObject* object) {
    if (object == nullptr) {
        return kInvalidHandle;
    }

    std::size_t index = 0;
    if (!free_slots_.empty()) {
        index = static_cast<std::size_t>(free_slots_.back()) - 1u;
        free_slots_.pop_back();
    } else {
        if (slots_.size() >= kMaxHandles) {
            return kInvalidHandle;
        }
        slots_.push_back(Slot{});
        index = slots_.size() - 1u;
    }

    Slot& slot = slots_[index];
    slot.object = object;
    object->AddRef();
    ++live_;
    return Compose(index, slot.generation);
}

KObject* HandleTable::Get(Handle handle) const noexcept {
    const std::uint32_t index_plus_one = handle & kHandleIndexMask;
    if (index_plus_one == 0) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(index_plus_one) - 1u;
    if (index >= slots_.size()) {
        return nullptr;
    }
    const Slot& slot = slots_[index];
    if (slot.object == nullptr) {
        return nullptr;
    }
    // The generation must match, or this handle was retired and the slot reused.
    if (((handle >> 16) & kHandleIndexMask) != slot.generation) {
        return nullptr;
    }
    return slot.object;
}

bool HandleTable::Close(Handle handle) {
    KObject* const object = Get(handle);
    if (object == nullptr) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(handle & kHandleIndexMask) - 1u;
    Retire(index);
    --live_;
    // Release last: the object may destroy itself here, and it must not do so
    // while the table still points at it.
    object->Release();
    return true;
}

void HandleTable::CloseAll() {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        KObject* const object = slots_[index].object;
        if (object == nullptr) {
            continue;
        }
        Retire(index);
        object->Release();
    }
    slots_.clear();
    free_slots_.clear();
    live_ = 0;
}

}  // namespace zlong::service
