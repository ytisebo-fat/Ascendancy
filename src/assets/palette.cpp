#include "assets/palette.h"

namespace asc {

Palette Palette::FromVga(const std::vector<uint8_t>& data) {
  Palette p;
  size_t n = data.size() / 3;
  if (n > 256) n = 256;
  for (size_t i = 0; i < 256; ++i) p.rgba[i] = 0xFF000000u;
  for (size_t i = 0; i < n; ++i) {
    uint32_t r = Expand6(data[i * 3]), g = Expand6(data[i * 3 + 1]), b = Expand6(data[i * 3 + 2]);
    p.rgba[i] = 0xFF000000u | b << 16 | g << 8 | r;
  }
  return p;
}

}  // namespace asc
