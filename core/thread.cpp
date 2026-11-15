#include "thread.hpp"

#include <new>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <pthread.h>
#endif

namespace nxp {

#ifdef __SWITCH__
static_assert(sizeof(::Thread) <= 64, "thread storage too small");

bool Thread::start(Fn fn, void* arg, size_t stack_size, int prio) {
    if (started_) return false;
    ::Thread* t = new (storage_)::Thread;
    stack_size = (stack_size + 0xFFF) & ~(size_t)0xFFF;
    if (R_FAILED(threadCreate(t, fn, arg, nullptr, stack_size, prio, -2))) return false;
    if (R_FAILED(threadStart(t))) {
        threadClose(t);
        return false;
    }
    started_ = true;
    return true;
}

void Thread::join() {
    if (!started_) return;
    ::Thread* t = (::Thread*)storage_;
    threadWaitForExit(t);
    threadClose(t);
    started_ = false;
}
#else
static_assert(sizeof(pthread_t) <= 64, "thread storage too small");

namespace {
struct Tramp {
    Thread::Fn fn;
    void* arg;
};
void* tramp(void* p) {
    Tramp t = *(Tramp*)p;
    delete (Tramp*)p;
    t.fn(t.arg);
    return nullptr;
}
}  // namespace

bool Thread::start(Fn fn, void* arg, size_t stack_size, int) {
    if (started_) return false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size < 65536) stack_size = 65536;
    pthread_attr_setstacksize(&attr, stack_size);
    Tramp* t = new Tramp{fn, arg};
    int rc = pthread_create((pthread_t*)storage_, &attr, tramp, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete t;
        return false;
    }
    started_ = true;
    return true;
}

void Thread::join() {
    if (!started_) return;
    pthread_join(*(pthread_t*)storage_, nullptr);
    started_ = false;
}
#endif

}  // namespace nxp
