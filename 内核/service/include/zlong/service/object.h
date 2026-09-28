// 烛龙 (ZhuLong) - the kernel object base.
//
// Everything a guest can hold a handle to derives from KObject. The base carries
// exactly two things: a type tag the handle table checks, and a reference count.
//
// Lifetime is by reference count alone -- there is no ownership tree. An object
// outlives the process that created it in every design that works, so ownership
// would only add a second, disagreeing notion of when an object dies.

#pragma once

#include <atomic>
#include <cstdint>

namespace zlong::service {

/// The type tag a handle table checks against. A mismatch is a miss, never a
/// reinterpretation.
enum class ObjectKind : std::uint8_t {
    Thread,
    Process,
    Session,
    Event,
    Mutex,
    Semaphore,
};

const char* ToString(ObjectKind kind) noexcept;

class KObject {
public:
    KObject(const KObject&) = delete;
    KObject& operator=(const KObject&) = delete;
    virtual ~KObject() = default;

    ObjectKind kind() const noexcept { return kind_; }

    std::int32_t ref_count() const noexcept { return refs_.load(std::memory_order_acquire); }

    /// Take a reference. Called by whoever hands the object out.
    void AddRef() noexcept;

    /// Drop a reference. Returns true when that call destroyed the object.
    bool Release() noexcept;

    /// Compile-time-checked type test, for a type that declares `kKind`.
    template <typename T>
    bool is() const noexcept {
        return kind_ == T::kKind;
    }

protected:
    explicit KObject(ObjectKind kind) noexcept : kind_(kind) {}

private:
    std::atomic<std::int32_t> refs_{1};
    const ObjectKind kind_;
};

}  // namespace zlong::service
