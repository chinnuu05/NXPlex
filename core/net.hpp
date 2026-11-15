// Minimal HTTP/1.1 client over BSD sockets + mbedTLS. Same code on Linux and
// on the Switch (libnx provides BSD sockets, devkitPro provides mbedTLS).
// One request per connection (Connection: close), streaming pull reads.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nxp {

struct Url {
    std::string scheme, host, path;  // path includes query, always starts with '/'
    uint16_t port = 0;
    bool tls = false;
};
bool parse_url(const std::string& s, Url& out);
std::string url_encode(const std::string& s);

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    uint32_t timeout_ms = 10000;  // connect and per-read timeout
    int max_redirects = 3;
};

struct HttpResponse {
    int status = 0;
    int64_t content_length = -1;
    std::string content_type;
    std::string error;  // transport-level error, empty on success
    Headers headers;
    std::string header(const char* name) const;  // case-insensitive, "" if absent
};

// Call once before any request (parses the embedded CA bundle, seeds the DRBG).
bool net_init();
void net_exit();

class HttpStream {
public:
    HttpStream();
    ~HttpStream();
    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;

    // Sends the request and reads the response headers (following redirects).
    // Returns false on transport errors (resp.error says why). Any HTTP status
    // counts as success; check resp.status.
    bool open(const HttpRequest& req, HttpResponse& resp);
    // Reads body bytes: >0 count, 0 at end of body, -1 on error.
    long read(void* buf, size_t n);
    void close();

private:
    struct Impl;
    Impl* p_;
};

// Whole body into `body` (fails if larger than max_bytes).
bool http_fetch(const HttpRequest& req, HttpResponse& resp, std::string* body, size_t max_bytes = 4u << 20);
// Streams the body through `cb`; cb returns false to abort.
bool http_fetch_cb(const HttpRequest& req, HttpResponse& resp, const std::function<bool(const char*, size_t)>& cb);

}  // namespace nxp
