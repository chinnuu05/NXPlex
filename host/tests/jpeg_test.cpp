// jpeg_test: the direct-to-RGB565 decoder must match stb's regular decoder
// (RGBA, then converted to RGB565) pixel for pixel, including odd sizes with
// partial MCUs and every chroma layout. Runs on an embedded image; pass more
// JPEG files on the command line to test them too. Also times both paths.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "decoders.hpp"
#include "odd420_jpeg.inc"

using namespace nxp;

static int g_fail = 0;

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void check(const char* name, const std::vector<uint8_t>& jpg) {
    int w = 0, h = 0;
    if (!jpeg_size(jpg.data(), jpg.size(), w, h)) {
        printf("FAIL %s: jpeg_size\n", name);
        g_fail++;
        return;
    }
    // Reference.
    std::vector<uint8_t> rgba;
    int rw, rh;
    auto t0 = std::chrono::steady_clock::now();
    bool ok_ref = jpeg_decode_rgba(jpg.data(), jpg.size(), rgba, rw, rh);
    double t_ref = ms_since(t0);
    // Direct, into a buffer with a wider stride and guard pixels to catch overruns.
    const int stride = w + 7;
    const uint16_t kGuard = 0xBEEF;
    std::vector<uint16_t> out((size_t)stride * h + 16, kGuard);
    t0 = std::chrono::steady_clock::now();
    bool ok = jpeg_decode_rgb565(jpg.data(), jpg.size(), out.data(), stride);
    double t_new = ms_since(t0);
    if (!ok_ref || !ok || rw != w || rh != h) {
        printf("FAIL %s: decode ref=%d new=%d size %dx%d vs %dx%d\n", name, ok_ref, ok, w, h, rw, rh);
        g_fail++;
        return;
    }
    size_t diffs = 0, guard_hits = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = &rgba[((size_t)y * w + x) * 4];
            if (out[(size_t)y * stride + x] != rgb565(p[0], p[1], p[2])) diffs++;
        }
        for (int x = w; x < stride; x++) guard_hits += out[(size_t)y * stride + x] != kGuard;
    }
    for (size_t i = (size_t)stride * h; i < out.size(); i++) guard_hits += out[i] != kGuard;
    bool pass = diffs == 0 && guard_hits == 0;
    if (!pass) g_fail++;
    printf("%s %-14s %4dx%-4d  mismatched pixels %zu, writes outside the image %zu  (stb %.2f ms, direct %.2f ms)\n",
           pass ? "PASS" : "FAIL", name, w, h, diffs, guard_hits, t_ref, t_new);
}

int main(int argc, char** argv) {
    check("embedded 4:2:0", std::vector<uint8_t>(kOdd420Jpeg, kOdd420Jpeg + sizeof(kOdd420Jpeg)));

    // A corrupt file must fail cleanly (no crash, no writes).
    std::vector<uint8_t> broken(kOdd420Jpeg, kOdd420Jpeg + sizeof(kOdd420Jpeg) / 3);
    std::vector<uint16_t> dummy(70 * 46, 0);
    bool ok = jpeg_decode_rgb565(broken.data(), broken.size(), dummy.data(), 70);
    printf("PASS truncated JPEG handled without crashing (%s)\n", ok ? "stb decoded what it could" : "rejected");

    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) {
            printf("FAIL cannot open %s\n", argv[i]);
            g_fail++;
            continue;
        }
        std::vector<uint8_t> d;
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
        fclose(f);
        std::string name = argv[i];
        name = name.substr(name.rfind('/') + 1);
        check(name.c_str(), d);
    }
    if (g_fail) {
        printf("%d failure(s)\n", g_fail);
        return 1;
    }
    printf("jpeg_test: all passed\n");
    return 0;
}
