#pragma once
#include <cstdint>
#include <vector>

#include "assets/palette.h"

namespace asc {

// SHP / TMP sprite container ("1.10").
//   char magic[4] = "1.10"
//   u32  count
//   struct { u32 imageOffset; u32 paletteOffset; } [count]   (palette: 0 = none)
// Image at imageOffset:
//   u16 height-1, u16 width-1, u32 reserved,
//   i32 xMin, yMin, xMax, yMax    bounding box (origin-relative);
//                                  0x7FFFxxxx values mark an empty frame
//   RLE stream, one row after another:
//     0x00        end of row
//     0x01 n      skip n pixels (transparent)
//     odd  b      copy (b>>1) literal pixels that follow
//     even b, v   repeat pixel v (b>>1) times
struct ShpFrame {
  int width = 0, height = 0;
  int xMin = 0, yMin = 0;           // placement relative to the sprite origin
  std::vector<uint8_t> pixels;      // palette indices
  std::vector<uint8_t> mask;        // 1 = opaque, 0 = transparent
  bool empty() const { return width == 0 || height == 0; }
};

class ShpFile {
 public:
  bool Parse(const std::vector<uint8_t>& data);
  size_t size() const { return frames_.size(); }
  const ShpFrame& frame(size_t i) const { return frames_[i]; }
  // Embedded palette for frame i, if any (none found in the shipped data).
  bool HasPalette(size_t i) const { return palettes_[i] != 0; }

  // Converts a frame to RGBA; transparent pixels get alpha 0.
  static std::vector<uint8_t> ToRGBA(const ShpFrame& f, const Palette& pal);

 private:
  std::vector<ShpFrame> frames_;
  std::vector<uint32_t> palettes_;
};

}  // namespace asc
