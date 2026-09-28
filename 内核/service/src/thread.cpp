#include "zlong/service/thread.h"

namespace zlong::service {

KThread::KThread(std::uint64_t tid, std::uint64_t owner_pid) noexcept
    : KObject(kKind), tid_(tid), owner_pid_(owner_pid) {}

KThread::State KThread::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void KThread::set_state(State state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

void KThread::set_start(std::uint64_t entry, std::uint64_t stack, std::uint64_t argument) {
    std::lock_guard<std::mutex> lock(mutex_);
    entry_ = entry;
    stack_ = stack;
    argument_ = argument;
}

std::uint64_t KThread::entry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entry_;
}

std::uint64_t KThread::stack() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stack_;
}

std::uint64_t KThread::argument() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return argument_;
}

cpu::Core* KThread::core() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_;
}

void KThread::set_core(cpu::Core* core) {
    std::lock_guard<std::mutex> lock(mutex_);
    core_ = core;
}

bool KThread::has_core() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_ != nullptr;
}

const char* ToString(KThread::State state) noexcept {
    switch (state) {
    case KThread::State::Created:
        return "created";
    case KThread::State::Ready:
        return "ready";
    case KThread::State::Running:
        return "running";
    case KThread::State::Waiting:
        return "waiting";
    case KThread::State::Terminated:
        return "terminated";
    }
    return "unknown";
}

}  // namespace zlong::service
