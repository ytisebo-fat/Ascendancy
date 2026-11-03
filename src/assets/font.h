#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace asc {

// FNT bitmap font (BIGFONT.FNT, SMFONT.FNT, INTRO.FNT).
//   char magic[2] = "1."; u16 0
//   u32 count (128), u32 height, u32 transparent value (255)
//   u32 glyphOffset[count]
// Glyph: u32 width, then height*width bytes; 0xFF = transparent, anything
// else = ink (the game recolours ink when drawing).
class FntFont {
 public:
  bool Parse(const std::vector<uint8_t>& data);
  int height() const { return height_; }
  int GlyphWidth(unsigned char c) const;
  int TextWidth(const std::string& s, int spacing = 1) const;
  // Calls plot(x, y) for every ink pixel of the text, origin top-left.
  template <typename Plot>
  int Draw(const std::string& s, int x, int y, Plot plot, int spacing = 1) const {
    for (unsigned char c : s) {
      if (c >= glyphs_.size()) c = '?';
      const Glyph& g = glyphs_[c];
      for (int gy = 0; gy < height_; ++gy)
        for (int gx = 0; gx < g.width; ++gx)
          if (g.ink[size_t(gy) * g.width + gx]) plot(x + gx, y + gy);
      x += g.width + spacing;
    }
    return x;
  }

 private:
  struct Glyph {
    int width = 0;
    std::vector<uint8_t> ink;  // 1 = ink
  };
  std::vector<Glyph> glyphs_;
  int height_ = 0;
};

}  // namespace asc
