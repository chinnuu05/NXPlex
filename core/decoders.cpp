#include "decoders.hpp"

#include <cstdlib>
#include <cstring>

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
