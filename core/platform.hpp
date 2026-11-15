// Tiny platform shims so the core builds unchanged on Linux (CLI/tests) and
// on the Switch (sysmodule).
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nxp {

uint64_t now_ms();  // monotonic milliseconds
uint64_t now_us();  // monotonic microseconds
void sleep_ms(uint32_t ms);
void random_bytes(void* out, size_t n);

// Logging. The sink is set once at startup by the platform layer.
typedef void (*LogFn)(const char* line);
void log_set_sink(LogFn fn);
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Bounded copy that always terminates and never splits a UTF-8 sequence
// (these strings end up on screen).
void copy_str(char* dst, size_t dst_size, const char* src);
template <size_t N>
inline void copy_str(char (&dst)[N], const char* src) { copy_str(dst, N, src); }

}  // namespace nxp
