#include "ipc_server.hpp"

#include <cstring>

namespace nxp {

Result IpcServer::init(const char* name, u32 max_sessions) {
    strncpy(name_, name, sizeof(name_) - 1);
    if (max_sessions + 1 > kMaxHandles) max_sessions = kMaxHandles - 1;
    max_sessions_ = max_sessions;
    Result rc = svcManageNamedPort(&handles_[0], name_, (s32)max_sessions);
    if (R_FAILED(rc)) {
        // A previous instance may have left the port registered: unregister and retry.
        Handle h;
        if (R_SUCCEEDED(svcManageNamedPort(&h, name_, 0))) svcCloseHandle(h);
        rc = svcManageNamedPort(&handles_[0], name_, (s32)max_sessions);
    }
    if (R_SUCCEEDED(rc)) count_ = 1;
    return rc;
}

void IpcServer::exit() {
    for (u32 i = 1; i < count_; i++) svcCloseHandle(handles_[i]);
    if (count_) {
        svcCloseHandle(handles_[0]);
        Handle h;
        if (R_SUCCEEDED(svcManageNamedPort(&h, name_, 0))) svcCloseHandle(h);
    }
    count_ = 0;
}

void IpcServer::close_session(u32 index) {
    if (index == 0 || index >= count_) return;
    svcCloseHandle(handles_[index]);
    for (u32 j = index; j + 1 < count_; j++) handles_[j] = handles_[j + 1];
    count_--;
}

bool IpcServer::process(u64 timeout_ns, const IpcHandler& handler) {
    s32 idx = -1;
    Result rc = svcWaitSynchronization(&idx, handles_, (s32)count_, timeout_ns);
    if (rc == KERNELRESULT(TimedOut)) return true;
    if (R_FAILED(rc)) return rc != KERNELRESULT(Cancelled);  // cancelled = asked to shut down
    if (idx < 0 || (u32)idx >= count_) return true;

    if (idx == 0) {
        Handle session;
        if (R_SUCCEEDED(svcAcceptSession(&session, handles_[0]))) {
            if (count_ < kMaxHandles && count_ - 1 < max_sessions_) handles_[count_++] = session;
            else svcCloseHandle(session);
        }
        return true;
    }

    Handle session = handles_[idx];
    s32 unused;
    rc = svcReplyAndReceive(&unused, &session, 1, 0, UINT64_MAX);
    if (R_FAILED(rc)) {  // client went away
        close_session((u32)idx);
        return true;
    }

    u8* tls = (u8*)armGetTls();
    HipcParsedRequest r = hipcParseRequest(tls);
    bool close = false;
    Result result = 0;
    u8 out[64];
    size_t out_size = 0;

    if (r.meta.type == CmifCommandType_Request || r.meta.type == CmifCommandType_RequestWithContext) {
        CmifInHeader* hdr = (CmifInHeader*)cmifGetAlignedDataStart(r.data.data_words, tls);
        u8* end = (u8*)r.data.data_words + r.meta.num_data_words * 4;
        if (!hdr || (u8*)(hdr + 1) > end || hdr->magic != CMIF_IN_HEADER_MAGIC) {
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        } else {
            IpcRequest req;
            req.cmd = hdr->command_id;
            // Copy inline input out of TLS: the reply is built in the same place.
            u8 in[64];
            size_t n = (size_t)(end - (u8*)(hdr + 1));
            if (n > sizeof(in)) n = sizeof(in);
            memcpy(in, hdr + 1, n);
            req.data = in;
            req.size = n;
            if (r.meta.num_send_buffers) {
                req.in_buf = (void*)hipcGetBufferAddress(&r.data.send_buffers[0]);
                req.in_buf_size = hipcGetBufferSize(&r.data.send_buffers[0]);
            }
            if (r.meta.num_recv_buffers) {
                req.out_buf = (void*)hipcGetBufferAddress(&r.data.recv_buffers[0]);
                req.out_buf_size = hipcGetBufferSize(&r.data.recv_buffers[0]);
            }
            result = handler(req, out, out_size);
            if (out_size > sizeof(out)) out_size = sizeof(out);
        }
    } else if (r.meta.type == CmifCommandType_Close) {
        close = true;
    } else {
        result = MAKERESULT(11, 403);
    }

    // Reply: CMIF out header + inline data.
    HipcRequest reply = hipcMakeRequestInline(tls, .type = CmifCommandType_Request,
                                              .num_data_words = (u32)((sizeof(CmifOutHeader) + out_size + 0x10 + 3) / 4));
    CmifOutHeader* oh = (CmifOutHeader*)cmifGetAlignedDataStart(reply.data_words, tls);
    oh->magic = CMIF_OUT_HEADER_MAGIC;
    oh->version = 0;
    oh->result = result;
    oh->token = 0;
    if (R_SUCCEEDED(result) && out_size) memcpy(oh + 1, out, out_size);

    rc = svcReplyAndReceive(&unused, &session, 0, session, 0);
    if (rc != KERNELRESULT(TimedOut) && R_FAILED(rc)) close = true;
    if (close) close_session((u32)idx);
    return true;
}

}  // namespace nxp
