#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nxp {

#ifdef __SWITCH__
uint64_t now_ms() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }
uint64_t now_us() { return armTicksToNs(armGetSystemTick()) / 1000ULL; }
void sleep_ms(uint32_t ms) { svcSleepThread((s64)ms * 1000000LL); }
void random_bytes(void* out, size_t n) { randomGet(out, n); }
#else
uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
void sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}
void random_bytes(void* out, size_t n) {
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = read(fd, (char*)out + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    if (got < n) abort();  // never hand out predictable "random" bytes
}
#endif

static LogFn g_log_sink = nullptr;
void log_set_sink(LogFn fn) { g_log_sink = fn; }

void logf(const char* fmt, ...) {
    if (!g_log_sink) return;
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_log_sink(line);
}

void copy_str(char* dst, size_t dst_size, const char* src) {
    if (!dst_size) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= dst_size) {
        n = dst_size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;  // UTF-8 boundary
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

}  // namespace nxp
