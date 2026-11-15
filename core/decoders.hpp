// Thin wrappers over the single-header decoders (stb_image JPEG, minimp3).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxp {

// Reads a JPEG's dimensions without decoding it.
bool jpeg_size(const uint8_t* data, size_t size, int& w, int& h);

// Decodes a JPEG straight to RGB565 rows (`stride_px` pixels apart) without a
// full-size RGB intermediate: only stb's YCbCr planes (~1.5 bytes/pixel) are
// allocated while decoding. Grayscale and YCbCr/RGB JPEGs; not CMYK.
bool jpeg_decode_rgb565(const uint8_t* data, size_t size, uint16_t* out, int stride_px);

// Reference decoder: one JPEG to RGBA8888 (alpha = 255). Used by tests.
bool jpeg_decode_rgba(const uint8_t* data, size_t size, std::vector<uint8_t>& out, int& w, int& h);

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();
    // Decodes all frames in the packet, appending interleaved s16 PCM to `pcm`.
    // rate/channels are those of the last decoded frame. Returns frames decoded.
    size_t decode(const uint8_t* data, size_t size, std::vector<int16_t>& pcm, int& rate, int& channels);
    void reset();

private:
    void* dec_;
};

// Converts interleaved s16 from (in_rate, in_ch) to stereo at out_rate with
// linear interpolation. Stateful across calls so packet joins are seamless.
class StereoResampler {
public:
    void reset() { pos_ = 0; have_last_ = false; }
    void process(const int16_t* in, size_t frames, int in_rate, int in_ch, int out_rate, std::vector<int16_t>& out);

private:
    double pos_ = 0;  // fractional read position relative to the current input block
    bool have_last_ = false;
    int16_t last_l_ = 0, last_r_ = 0;
};

}  // namespace nxp
