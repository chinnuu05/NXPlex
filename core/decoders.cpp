#include "decoders.hpp"

#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)
#define STBI_NEON  // the Switch: SIMD IDCT, upsampling and color conversion
#endif
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "third_party/stb_image.h"

#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace nxp {

bool jpeg_size(const uint8_t* data, size_t size, int& w, int& h) {
    int comp = 0;
    return stbi_info_from_memory(data, (int)size, &w, &h, &comp) && w > 0 && h > 0;
}

// Mirrors stb's load_jpeg_image(), but instead of building an RGB(A) image it
// upsamples and color-converts one row at a time into RGB565.
static bool jpeg_rows_to_rgb565(stbi__jpeg* z, uint16_t* out, int stride_px) {
    const int img_n = z->s->img_n;
    if (img_n != 1 && img_n != 3) return false;  // CMYK etc.: not produced by Plex
    const int is_rgb = img_n == 3 && (z->rgb == 3 || (z->app14_color_transform == 0 && !z->jfif));
    const unsigned w = z->s->img_x, h = z->s->img_y;

    stbi__resample res[3];
    for (int k = 0; k < img_n; ++k) {
        stbi__resample* r = &res[k];
        z->img_comp[k].linebuf = (stbi_uc*)stbi__malloc(w + 3);  // room for 4x upsampling off the edge
        if (!z->img_comp[k].linebuf) return false;
        r->hs = z->img_h_max / z->img_comp[k].h;
        r->vs = z->img_v_max / z->img_comp[k].v;
        r->ystep = r->vs >> 1;
        r->w_lores = (w + r->hs - 1) / r->hs;
        r->ypos = 0;
        r->line0 = r->line1 = z->img_comp[k].data;
        if (r->hs == 1 && r->vs == 1) r->resample = resample_row_1;
        else if (r->hs == 1 && r->vs == 2) r->resample = stbi__resample_row_v_2;
        else if (r->hs == 2 && r->vs == 1) r->resample = stbi__resample_row_h_2;
        else if (r->hs == 2 && r->vs == 2) r->resample = z->resample_row_hv_2_kernel;
        else r->resample = stbi__resample_row_generic;
    }
    // One RGBA row (step 4 is the SIMD-accelerated color conversion path).
    stbi_uc* rgba = (stbi_uc*)stbi__malloc((size_t)w * 4);
    if (!rgba) return false;

    stbi_uc* c[3] = {nullptr, nullptr, nullptr};
    for (unsigned j = 0; j < h; ++j) {
        for (int k = 0; k < img_n; ++k) {
            stbi__resample* r = &res[k];
            int y_bot = r->ystep >= (r->vs >> 1);
            c[k] = r->resample(z->img_comp[k].linebuf, y_bot ? r->line1 : r->line0, y_bot ? r->line0 : r->line1,
                               r->w_lores, r->hs);
            if (++r->ystep >= r->vs) {
                r->ystep = 0;
                r->line0 = r->line1;
                if (++r->ypos < z->img_comp[k].y) r->line1 += z->img_comp[k].w2;
            }
        }
        uint16_t* dst = out + (size_t)j * (size_t)stride_px;
        if (img_n == 1) {
            for (unsigned i = 0; i < w; ++i) dst[i] = rgb565(c[0][i], c[0][i], c[0][i]);
        } else if (is_rgb) {
            for (unsigned i = 0; i < w; ++i) dst[i] = rgb565(c[0][i], c[1][i], c[2][i]);
        } else {
            z->YCbCr_to_RGB_kernel(rgba, c[0], c[1], c[2], (int)w, 4);
            for (unsigned i = 0; i < w; ++i) dst[i] = rgb565(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
        }
    }
    STBI_FREE(rgba);
    return true;
}

bool jpeg_decode_rgb565(const uint8_t* data, size_t size, uint16_t* out, int stride_px) {
    stbi__context s;
    stbi__start_mem(&s, data, (int)size);
    stbi__jpeg* z = (stbi__jpeg*)stbi__malloc(sizeof(stbi__jpeg));
    if (!z) return false;
    memset(z, 0, sizeof(*z));
    z->s = &s;
    stbi__setup_jpeg(z);
    z->s->img_n = 0;  // makes cleanup safe if decoding fails early
    bool ok = stbi__decode_jpeg_image(z) && jpeg_rows_to_rgb565(z, out, stride_px);
    stbi__cleanup_jpeg(z);  // frees component planes and line buffers
    STBI_FREE(z);
    return ok;
}

bool jpeg_decode_rgba(const uint8_t* data, size_t size, std::vector<uint8_t>& out, int& w, int& h) {
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
    if (!px) return false;
    out.assign(px, px + (size_t)w * (size_t)h * 4);
    stbi_image_free(px);
    return true;
}

Mp3Decoder::Mp3Decoder() : dec_(malloc(sizeof(mp3dec_t))) { reset(); }
Mp3Decoder::~Mp3Decoder() { free(dec_); }
void Mp3Decoder::reset() { mp3dec_init((mp3dec_t*)dec_); }

size_t Mp3Decoder::decode(const uint8_t* data, size_t size, std::vector<int16_t>& pcm, int& rate, int& channels) {
    size_t total = 0;
    mp3d_sample_t buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
    while (size > 0) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame((mp3dec_t*)dec_, data, (int)size, buf, &info);
        if (info.frame_bytes == 0) break;  // no more frames in this packet
        data += info.frame_bytes;
        size -= (size_t)info.frame_bytes;
        if (n <= 0) continue;
        rate = info.hz;
        channels = info.channels;
        pcm.insert(pcm.end(), buf, buf + (size_t)n * (size_t)info.channels);
        total += (size_t)n;
    }
    return total;
}

void StereoResampler::process(const int16_t* in, size_t frames, int in_rate, int in_ch, int out_rate,
                              std::vector<int16_t>& out) {
    if (!frames || in_ch < 1) return;
    auto L = [&](size_t i) { return in[i * (size_t)in_ch]; };
    auto R = [&](size_t i) { return in[i * (size_t)in_ch + (in_ch > 1 ? 1 : 0)]; };
    if (in_rate == out_rate) {
        for (size_t i = 0; i < frames; i++) {
            out.push_back(L(i));
            out.push_back(R(i));
        }
        last_l_ = L(frames - 1);
        last_r_ = R(frames - 1);
        have_last_ = true;
        return;
    }
    // Positions are relative to this block; index -1 is the previous block's last sample.
    double step = (double)in_rate / (double)out_rate;
    double p = have_last_ ? pos_ : 0.0;
    while (p < (double)frames - 1.0 || (p < 0 && have_last_)) {
        long i0 = (long)(p < 0 ? -1 : (long)p);
        double f = p - (double)i0;
        int a_l = i0 < 0 ? last_l_ : L((size_t)i0), a_r = i0 < 0 ? last_r_ : R((size_t)i0);
        int b_l = L((size_t)(i0 + 1)), b_r = R((size_t)(i0 + 1));
        out.push_back((int16_t)(a_l + (b_l - a_l) * f));
        out.push_back((int16_t)(a_r + (b_r - a_r) * f));
        p += step;
    }
    pos_ = p - (double)frames;  // carries into the next block (in [-1, step-1))
    last_l_ = L(frames - 1);
    last_r_ = R(frames - 1);
    have_last_ = true;
}

}  // namespace nxp
