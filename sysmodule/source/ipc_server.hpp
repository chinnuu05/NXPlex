// Minimal HIPC/CMIF server on a kernel named port (no sm registration
// needed). Requests carry small inline data; large payloads use HIPC
// mapped buffers (type A in / type B out).
#pragma once
#include <switch.h>

#include <cstddef>
#include <functional>

namespace nxp {

struct IpcRequest {
    u32 cmd = 0;
    const u8* data = nullptr;  // inline input (after the CMIF header)
    size_t size = 0;
    void* in_buf = nullptr;    // first type-A buffer, if any
    size_t in_buf_size = 0;
    void* out_buf = nullptr;   // first type-B buffer, if any
    size_t out_buf_size = 0;
};

// Handler: fill up to 64 bytes of inline output and set out_size.
using IpcHandler = std::function<Result(const IpcRequest& req, u8* out, size_t& out_size)>;

class IpcServer {
public:
    Result init(const char* name, u32 max_sessions);
    void exit();
    // Waits up to timeout_ns for a connection or request and handles it.
    // Returns false if the server can't continue.
    bool process(u64 timeout_ns, const IpcHandler& handler);
    u32 session_count() const { return count_ > 0 ? count_ - 1 : 0; }

private:
    void close_session(u32 index);
    static const u32 kMaxHandles = 8;
    char name_[12] = {};
    Handle handles_[kMaxHandles] = {};  // [0] = port
    u32 count_ = 0, max_sessions_ = 0;
};

}  // namespace nxp
