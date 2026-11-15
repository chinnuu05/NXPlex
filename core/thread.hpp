// Thread wrapper with an explicit stack size (the overlay only has a 6 MB
// heap, so default 1 MB+ pthread stacks are not acceptable).
#pragma once
#include <cstddef>

namespace nxp {

class Thread {
public:
    typedef void (*Fn)(void*);
    Thread() = default;
    ~Thread() { join(); }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // prio: Switch thread priority (0x2C is the libnx default; higher number = lower priority).
    bool start(Fn fn, void* arg, size_t stack_size, int prio = 0x2C);
    void join();
    bool running() const { return started_; }

private:
    bool started_ = false;
    alignas(16) unsigned char storage_[64];  // platform handle
};

}  // namespace nxp
