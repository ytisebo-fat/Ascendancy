#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace asc {

// VGA palette: 256 x RGB, 6 bits per channel (0..63), 768 bytes.
// Some files (subfont.pal) hold fewer entries; missing entries stay black.
struct Palette {
  std::array<uint32_t, 256> rgba{};  // 0xAABBGGRR (little-endian RGBA bytes)

  static Palette FromVga(const std::vector<uint8_t>& data);
  static uint8_t Expand6(uint8_t v) { return uint8_t((v & 63) * 255 / 63); }
};

}  // namespace asc
