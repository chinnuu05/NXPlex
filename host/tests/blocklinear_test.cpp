// The PiP display writes frames straight into libnx's block-linear
// framebuffer. Check bl_offset() against a transcription of libnx's own
// _convertToBlocklinear()/_convertGobTo16Bx2() (nx/source/display/framebuffer.c).
#include <cstdio>
#include <cstring>
#include <vector>

#include "blocklinear.hpp"

using namespace nxp;

// libnx reference (block_height_log2 = 4).
static void libnx_to_blocklinear(uint8_t* out, const uint8_t* in, uint32_t stride, uint32_t height) {
    const uint32_t log2 = 4, bh_gobs = 1u << log2, bh_px = 8u << log2;
    const uint32_t width_blocks = stride >> 6, height_blocks = (height + bh_px - 1) >> (3 + log2);
    uint8_t* outgob = out;
    for (uint32_t by = 0; by < height_blocks; by++)
        for (uint32_t bx = 0; bx < width_blocks; bx++)
            for (uint32_t gy = 0; gy < bh_gobs; gy++) {
                const uint32_t x = bx * 64, y = by * bh_px + gy * 8;
                if (y < height) {
                    const uint8_t* ingob = in + y * stride + x;
                    for (uint32_t i = 0; i < 32; i++) {
                        const uint32_t yy = ((i >> 1) & 0x06) | (i & 0x01);
                        const uint32_t xx = ((i << 3) & 0x10) | ((i << 1) & 0x20);
                        memcpy(outgob + i * 16, ingob + yy * stride + xx, 16);
                    }
                }
                outgob += 512;
            }
}

int main() {
    int fails = 0;
    // Sizes used by NXPlex (and awkward ones): width in pixels at 2 bytes/pixel.
    const int sizes[][2] = {{256, 144}, {384, 216}, {512, 288}, {640, 360}, {852, 480}, {288, 216}, {70, 46}};
    for (auto& sz : sizes) {
        uint32_t w = sz[0], h = sz[1];
        uint32_t stride = (w * 2 + 63) & ~63u, h_al = (h + 127) & ~127u;
        std::vector<uint8_t> linear((size_t)stride * h_al), ref((size_t)stride * h_al), mine((size_t)stride * h_al);
        for (size_t i = 0; i < linear.size(); i++) linear[i] = (uint8_t)(i * 2654435761u >> 13);
        libnx_to_blocklinear(ref.data(), linear.data(), stride, h);
        // NXPlex's path: copy each row in 16-byte pieces through bl_offset().
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t xb = 0; xb < w * 2; xb += 16) {
                uint32_t n = w * 2 - xb < 16 ? w * 2 - xb : 16;
                memcpy(mine.data() + bl_offset(xb, y, stride >> 6), linear.data() + (size_t)y * stride + xb, n);
            }
        // Compare every pixel that belongs to the image.
        size_t bad = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t xb = 0; xb < w * 2; xb++) {
                uint32_t o = bl_offset(xb, y, stride >> 6);
                if (ref[o] != linear[(size_t)y * stride + xb] || mine[o] != ref[o]) bad++;
            }
        printf("%s %4ux%-4u stride %4u: %zu mismatched bytes\n", bad ? "FAIL" : "PASS", w, h, stride, bad);
        fails += bad != 0;
    }
    printf(fails ? "blocklinear_test: FAILED\n" : "blocklinear_test: all passed\n");
    return fails ? 1 : 0;
}
