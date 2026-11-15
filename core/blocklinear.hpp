// Tegra block-linear addressing, as libnx's framebufferCreate() lays out
// surfaces: 64-byte x 8-row GOBs of 512 bytes, 16 GOBs (128 rows) per block,
// blocks row-major across the (64-byte aligned) stride. Lets the PiP display
// write frames straight into the framebuffer without a linear shadow copy.
#pragma once
#include <cstdint>

namespace nxp {

// Byte offset of byte column xb, row y. width_blocks = stride_bytes / 64.
inline uint32_t bl_offset(uint32_t xb, uint32_t y, uint32_t width_blocks) {
    uint32_t gob = ((y >> 7) * width_blocks + (xb >> 6)) * 16 + ((y >> 3) & 15);
    uint32_t r = y & 7, xg = xb & 63;
    uint32_t chunk = (r & 1) | ((xg >> 3) & 2) | ((r << 1) & 4) | ((r << 1) & 8) | ((xg >> 1) & 16);
    return gob * 512 + chunk * 16 + (xg & 15);
}

}  // namespace nxp
