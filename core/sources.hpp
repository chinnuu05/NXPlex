// ByteSource implementations: local file and HTTP stream.
#pragma once
#include <cstdio>
#include <string>

#include "mkv.hpp"
#include "net.hpp"

namespace nxp {

class FileSource : public ByteSource {
public:
    ~FileSource() override { close(); }
    bool open(const std::string& path) {
        close();
        f_ = fopen(path.c_str(), "rb");
        size_ = -1;
        if (f_) setvbuf(f_, nullptr, _IOFBF, 16 * 1024);
        return f_ != nullptr;
    }
    void close() {
        if (f_) fclose(f_);
        f_ = nullptr;
    }
    long read(void* buf, size_t n) override {
        if (!f_) return -1;
        size_t r = fread(buf, 1, n, f_);
        if (r == 0) return ferror(f_) ? -1 : 0;
        return (long)r;
    }
    bool seek(uint64_t pos) override {
        if (!f_) return false;
        clearerr(f_);
        return fseek(f_, (long)pos, SEEK_SET) == 0;
    }
    int64_t size() override {
        if (!f_) return -1;
        if (size_ < 0) {
            long cur = ftell(f_);
            if (fseek(f_, 0, SEEK_END) == 0) size_ = ftell(f_);
            fseek(f_, cur, SEEK_SET);
        }
        return size_;
    }

private:
    FILE* f_ = nullptr;
    int64_t size_ = -1;
};

class HttpSource : public ByteSource {
public:
    bool open(const HttpRequest& req, HttpResponse& resp) { return s_.open(req, resp); }
    long read(void* buf, size_t n) override { return s_.read(buf, n); }
    void close() { s_.close(); }

private:
    HttpStream s_;
};

}  // namespace nxp
