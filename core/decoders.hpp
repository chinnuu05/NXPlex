// Thin wrappers over the single-header decoders (stb_image JPEG, minimp3).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxp {

// Decodes one JPEG frame to RGBA8888 (alpha = 255). Reuses `out`.
bool jpeg_decode_rgba(const uint8_t* data, size_t size, std::vector<uint8_t>& out, int& w, int& h);

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
