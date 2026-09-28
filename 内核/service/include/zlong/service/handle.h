// 烛龙 (ZhuLong) - the handle table.
//
// A handle carries a slot index *and* a generation. The generation is not an
// optimisation: without it, closing a handle and letting the slot be reused
// makes the stale handle alias whatever object landed there next -- a silent
// wrong answer of exactly the kind this kernel refuses to give. With it, a stale
// handle always misses.
//
// The generation is 16 bits, so after 65536 close/reuse cycles on one slot an
// ancient handle could match again. That window is documented rather than
// closed: closing it would mean never reusing slots, which leaks a table entry
// per handle ever handed out.
//
// The table is not internally locked. One table belongs to one process, and the
// process's own lock is the caller's business -- the service layer takes it at
// the syscall boundary, once, rather than per lookup.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "zlong/service/object.h"

namespace zlong::service {

using Handle = std::uint32_t;

/// Zero is never a valid handle, so a zeroed structure cannot name an object.
inline constexpr Handle kInvalidHandle = 0;
inline constexpr std::uint32_t kHandleIndexMask = 0xFFFF;
/// One slot index per handle; index 0 is reserved as "invalid".
inline constexpr std::uint32_t kMaxHandles = kHandleIndexMask;

class HandleTable {
public:
    HandleTable() = default;
    ~HandleTable();

    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    /// Hand out a handle for `object`, taking a reference. Returns
    /// kInvalidHandle when the object is null or the table is full.
    Handle Allocate(KObject* object);

    /// The object a handle names, or nullptr -- for a dead handle, a handle
    /// never handed out, or one whose generation no longer matches.
    KObject* Get(Handle handle) const noexcept;

    /// As Get, but only when the object's kind matches.
    template <typename T>
    T* GetAs(Handle handle) const noexcept {
        KObject* object = Get(handle);
        if (object == nullptr || !object->is<T>()) {
            return nullptr;
        }
        return static_cast<T*>(object);
    }

    /// Drop the handle's reference. False when the handle was already dead.
    bool Close(Handle handle);

    /// Close every handle. Used when the owning process dies, and by the
    /// destructor, so no reference is ever leaked by forgetting a close.
    void CloseAll();

    std::size_t live_handles() const noexcept { return live_; }
    std::size_t slot_count() const noexcept { return slots_.size(); }

private:
    struct Slot {
        KObject* object = nullptr;
        std::uint32_t generation = 1;
    };

    /// Retire the slot at `index`: bump its generation so every handle that
    /// named the old object misses, then make the slot available again.
    void Retire(std::size_t index) noexcept;

    std::vector<Slot> slots_;
    /// One-based slot indices, so a zeroed entry cannot look like a free slot.
    std::vector<std::uint32_t> free_slots_;
    std::size_t live_ = 0;
};

}  // namespace zlong::service
