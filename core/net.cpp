#include "net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_NET_* codes only
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "platform.hpp"

#include "cacerts.inc"

namespace nxp {

// ---------------------------------------------------------------- utilities

static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static bool ieq(const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool parse_url(const std::string& s, Url& out) {
    size_t p = s.find("://");
    if (p == std::string::npos) return false;
    out.scheme = lower(s.substr(0, p));
    if (out.scheme == "http") out.tls = false;
    else if (out.scheme == "https") out.tls = true;
    else return false;
    size_t hs = p + 3;
    size_t pe = s.find_first_of("/?#", hs);
    std::string hostport = s.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    if (hostport.empty() || hostport.find('@') != std::string::npos) return false;
    out.port = out.tls ? 443 : 80;
    if (hostport[0] == '[') return false;  // IPv6 literals: not needed for Plex
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        char* end = nullptr;
        long port = strtol(hostport.c_str() + colon + 1, &end, 10);
        if (!end || *end || port <= 0 || port > 65535) return false;
        out.port = (uint16_t)port;
        hostport.resize(colon);
    }
    out.host = lower(hostport);
    if (out.host.empty()) return false;
    std::string rest = pe == std::string::npos ? "" : s.substr(pe);
    size_t frag = rest.find('#');
    if (frag != std::string::npos) rest.resize(frag);
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    out.path = rest;
    return true;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            o.push_back((char)c);
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

std::string HttpResponse::header(const char* name) const {
    for (auto& h : headers)
        if (ieq(h.first, name)) return h.second;
    return "";
}

// ---------------------------------------------------------------- TLS globals

static bool g_net_ready = false;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_ca;
static std::mutex g_drbg_mutex;

static int locked_rng(void* ctx, unsigned char* out, size_t len) {
    std::lock_guard<std::mutex> lk(g_drbg_mutex);
    return mbedtls_ctr_drbg_random(ctx, out, len);
}

bool net_init() {
    if (g_net_ready) return true;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_ca);
    static const char pers[] = "nxplex";
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char*)pers, sizeof(pers));
    if (rc != 0) {
        logf("net: drbg seed failed -0x%x", -rc);
        return false;
    }
    rc = mbedtls_x509_crt_parse(&g_ca, (const unsigned char*)kCaBundle, sizeof(kCaBundle));
    if (rc < 0) {
        logf("net: CA bundle parse failed -0x%x", -rc);
        return false;
    }
    g_net_ready = true;
    return true;
}

void net_exit() {
    if (!g_net_ready) return;
    mbedtls_x509_crt_free(&g_ca);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);
    g_net_ready = false;
}

// ---------------------------------------------------------------- connection

namespace {

struct Conn {
    int fd = -1;
    bool tls = false;
    bool ssl_ready = false;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
        int fd = *(int*)ctx;
        ssize_t r = ::send(fd, buf, len, 0);
        if (r >= 0) return (int)r;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
        int fd = *(int*)ctx;
        ssize_t r = ::recv(fd, buf, len, 0);
        if (r >= 0) return (int)r;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;  // includes SO_RCVTIMEO timeouts
    }

    bool connect(const Url& u, uint32_t timeout_ms, std::string& err) {
        char port[8];
        snprintf(port, sizeof(port), "%u", u.port);
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        int gai = getaddrinfo(u.host.c_str(), port, &hints, &res);
        if (gai != 0 || !res) {
            err = "DNS lookup failed for " + u.host;
            return false;
        }
        bool ok = false;
        for (struct addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                struct pollfd pfd = {fd, POLLOUT, 0};
                rc = -1;
                if (poll(&pfd, 1, (int)timeout_ms) == 1) {
                    int soerr = 0;
                    socklen_t sl = sizeof(soerr);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                    if (soerr == 0) rc = 0;
                }
            }
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            if (rc == 0) {
                ok = true;
            } else {
                ::close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        if (!ok) {
            err = "could not connect to " + u.host + ":" + std::to_string(u.port);
            return false;
        }
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        tls = u.tls;
        if (!tls) return true;
        if (!g_net_ready) {
            err = "net_init() not called";
            return false;
        }
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        ssl_ready = true;
        int rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc == 0) {
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf, &g_ca, nullptr);
            mbedtls_ssl_conf_rng(&conf, locked_rng, &g_drbg);
            rc = mbedtls_ssl_setup(&ssl, &conf);
        }
        if (rc == 0) rc = mbedtls_ssl_set_hostname(&ssl, u.host.c_str());
        if (rc == 0) {
            mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, nullptr);
            while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
                if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) break;
            }
        }
        if (rc != 0) {
            char buf[128];
            mbedtls_strerror(rc, buf, sizeof(buf));
            err = "TLS handshake with " + u.host + " failed: " + buf;
            uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
            if (flags && flags != (uint32_t)-1) {
                char vb[256];
                mbedtls_x509_crt_verify_info(vb, sizeof(vb), "", flags);
                for (char* q = vb; *q; q++) if (*q == '\n') *q = ' ';
                err += std::string(" (") + vb + ")";
            }
            return false;
        }
        return true;
    }

    bool write_all(const char* data, size_t n) {
        while (n) {
            int r;
            if (tls) {
                r = mbedtls_ssl_write(&ssl, (const unsigned char*)data, n);
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            } else {
                r = (int)::send(fd, data, n, 0);
                if (r < 0 && errno == EINTR) continue;
            }
            if (r <= 0) return false;
            data += r;
            n -= (size_t)r;
        }
        return true;
    }

    // >0 bytes, 0 = peer closed, -1 = error/timeout
    long read_some(char* buf, size_t n) {
        for (;;) {
            if (tls) {
                int r = mbedtls_ssl_read(&ssl, (unsigned char*)buf, n);
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return 0;
                return r < 0 ? -1 : r;
            }
            ssize_t r = ::recv(fd, buf, n, 0);
            if (r < 0 && errno == EINTR) continue;
            return r < 0 ? -1 : (long)r;
        }
    }

    void close() {
        if (ssl_ready) {
            if (fd >= 0) mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            ssl_ready = false;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

}  // namespace

// ---------------------------------------------------------------- HttpStream

struct HttpStream::Impl {
    Conn conn;
    char buf[4096];
    size_t pos = 0, len = 0;
    enum class Body { None, Length, Chunked, UntilClose } mode = Body::None;
    int64_t remaining = 0;  // Length: bytes left; Chunked: bytes left in chunk
    bool done = false, failed = false;

    // Returns -1 on error, 0 on EOF, else 1.
    int fill() {
        if (pos < len) return 1;
        long r = conn.read_some(buf, sizeof(buf));
        if (r < 0) return -1;
        if (r == 0) return 0;
        pos = 0;
        len = (size_t)r;
        return 1;
    }

    bool read_line(std::string& line, size_t max = 8192) {
        line.clear();
        for (;;) {
            if (fill() != 1) return false;
            char* start = buf + pos;
            char* nl = (char*)memchr(start, '\n', len - pos);
            size_t take = nl ? (size_t)(nl - start) + 1 : len - pos;
            line.append(start, take);
            pos += take;
            if (line.size() > max) return false;
            if (nl) {
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                return true;
            }
        }
    }

    // Raw body bytes from buffer/socket.
    long raw_read(char* out, size_t n) {
        int f = fill();
        if (f <= 0) return f;
        size_t take = len - pos < n ? len - pos : n;
        memcpy(out, buf + pos, take);
        pos += take;
        return (long)take;
    }
};

HttpStream::HttpStream() : p_(new Impl) {}
HttpStream::~HttpStream() {
    close();
    delete p_;
}

void HttpStream::close() {
    p_->conn.close();
    p_->pos = p_->len = 0;
    p_->done = true;
}

static std::string resolve_location(const Url& base, const std::string& loc) {
    if (loc.find("://") != std::string::npos) return loc;
    std::string root = base.scheme + "://" + base.host + ":" + std::to_string(base.port);
    if (!loc.empty() && loc[0] == '/') return root + loc;
    std::string dir = base.path.substr(0, base.path.find('?'));
    dir = dir.substr(0, dir.rfind('/') + 1);
    return root + dir + loc;
}

bool HttpStream::open(const HttpRequest& req_in, HttpResponse& resp) {
    HttpRequest req = req_in;
    for (int hop = 0;; hop++) {
        close();
        Impl& p = *p_;
        p.done = false;
        p.failed = false;
        p.mode = Impl::Body::None;
        resp = HttpResponse();

        Url u;
        if (!parse_url(req.url, u)) {
            resp.error = "bad URL: " + req.url;
            return false;
        }
        if (!p.conn.connect(u, req.timeout_ms, resp.error)) {
            close();
            return false;
        }

        std::string head;
        head.reserve(512);
        head += req.method + " " + u.path + " HTTP/1.1\r\n";
        head += "Host: " + u.host;
        if ((u.tls && u.port != 443) || (!u.tls && u.port != 80)) head += ":" + std::to_string(u.port);
        head += "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
        bool has_ua = false;
        for (auto& h : req.headers) {
            if (ieq(h.first, "user-agent")) has_ua = true;
            head += h.first + ": " + h.second + "\r\n";
        }
        if (!has_ua) head += "User-Agent: NXPlex\r\n";
        if (!req.body.empty() || req.method == "POST" || req.method == "PUT")
            head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        head += "\r\n";
        if (!p.conn.write_all(head.data(), head.size()) ||
            (!req.body.empty() && !p.conn.write_all(req.body.data(), req.body.size()))) {
            resp.error = "failed to send request to " + u.host;
            close();
            return false;
        }

        std::string line;
        // Skip interim 1xx responses.
        for (;;) {
            if (!p.read_line(line)) {
                resp.error = "no response from " + u.host;
                close();
                return false;
            }
            if (line.compare(0, 5, "HTTP/") != 0 || line.size() < 12) {
                resp.error = "malformed status line from " + u.host;
                close();
                return false;
            }
            resp.status = atoi(line.c_str() + 9);
            resp.headers.clear();
            for (;;) {
                if (!p.read_line(line)) {
                    resp.error = "truncated headers from " + u.host;
                    close();
                    return false;
                }
                if (line.empty()) break;
                size_t c = line.find(':');
                if (c == std::string::npos) continue;
                size_t v = c + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) v++;
                resp.headers.emplace_back(line.substr(0, c), line.substr(v));
            }
            if (resp.status >= 200 || resp.status == 101) break;
        }

        resp.content_type = resp.header("content-type");
        std::string te = lower(resp.header("transfer-encoding"));
        std::string cl = resp.header("content-length");
        if (!cl.empty()) resp.content_length = strtoll(cl.c_str(), nullptr, 10);

        bool redirect = (resp.status == 301 || resp.status == 302 || resp.status == 303 || resp.status == 307 ||
                         resp.status == 308);
        std::string loc = resp.header("location");
        if (redirect && !loc.empty() && hop < req.max_redirects) {
            req.url = resolve_location(u, loc);
            if (resp.status == 303 || ((resp.status == 301 || resp.status == 302) && req.method == "POST")) {
                req.method = "GET";
                req.body.clear();
            }
            continue;
        }

        if (req.method == "HEAD" || resp.status == 204 || resp.status == 304) {
            p.mode = Impl::Body::None;
            p.done = true;
        } else if (te.find("chunked") != std::string::npos) {
            p.mode = Impl::Body::Chunked;
            p.remaining = 0;
        } else if (resp.content_length >= 0) {
            p.mode = Impl::Body::Length;
            p.remaining = resp.content_length;
            p.done = p.remaining == 0;
        } else {
            p.mode = Impl::Body::UntilClose;
        }
        return true;
    }
}

long HttpStream::read(void* out_v, size_t n) {
    Impl& p = *p_;
    char* out = (char*)out_v;
    if (p.failed) return -1;
    if (p.done || n == 0) return 0;
    switch (p.mode) {
    case Impl::Body::None:
        return 0;
    case Impl::Body::Length: {
        size_t want = (int64_t)n < p.remaining ? n : (size_t)p.remaining;
        long r = p.raw_read(out, want);
        if (r <= 0) {
            p.failed = true;  // closed before Content-Length bytes arrived
            return -1;
        }
        p.remaining -= r;
        if (p.remaining == 0) p.done = true;
        return r;
    }
    case Impl::Body::UntilClose: {
        long r = p.raw_read(out, n);
        if (r == 0) p.done = true;
        if (r < 0) p.failed = true;
        return r;
    }
    case Impl::Body::Chunked: {
        std::string line;
        if (p.remaining == 0) {
            if (!p.read_line(line, 256)) {
                p.failed = true;
                return -1;
            }
            if (line.empty() && !p.read_line(line, 256)) {  // CRLF after previous chunk
                p.failed = true;
                return -1;
            }
            p.remaining = strtoll(line.c_str(), nullptr, 16);
            if (p.remaining <= 0) {
                // Last chunk; trailers are ignored.
                while (p.read_line(line, 8192) && !line.empty()) {
                }
                p.done = true;
                return 0;
            }
        }
        size_t want = (int64_t)n < p.remaining ? n : (size_t)p.remaining;
        long r = p.raw_read(out, want);
        if (r <= 0) {
            p.failed = true;
            return -1;
        }
        p.remaining -= r;
        return r;
    }
    }
    return -1;
}

bool http_fetch_cb(const HttpRequest& req, HttpResponse& resp, const std::function<bool(const char*, size_t)>& cb) {
    HttpStream s;
    if (!s.open(req, resp)) return false;
    char buf[2048];
    for (;;) {
        long r = s.read(buf, sizeof(buf));
        if (r == 0) return true;
        if (r < 0) {
            resp.error = "connection lost while reading response";
            return false;
        }
        if (!cb(buf, (size_t)r)) {
            if (resp.error.empty()) resp.error = "aborted";
            return false;
        }
    }
}

bool http_fetch(const HttpRequest& req, HttpResponse& resp, std::string* body, size_t max_bytes) {
    if (body) body->clear();
    return http_fetch_cb(req, resp, [&](const char* d, size_t n) {
        if (!body) return true;
        if (body->size() + n > max_bytes) {
            resp.error = "response too large";
            return false;
        }
        body->append(d, n);
        return true;
    });
}

}  // namespace nxp
