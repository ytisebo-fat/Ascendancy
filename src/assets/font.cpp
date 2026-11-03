#include "assets/font.h"

namespace asc {
namespace {
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}  // namespace

bool FntFont::Parse(const std::vector<uint8_t>& d) {
  glyphs_.clear();
  if (d.size() < 16 || d[0] != '1' || d[1] != '.') return false;
  uint32_t count = Le32(&d[4]);
  uint32_t height = Le32(&d[8]);
  uint32_t transparent = Le32(&d[12]);
  if (count == 0 || count > 256 || height == 0 || height > 64 || 16 + count * 4 > d.size()) return false;
  height_ = int(height);
  glyphs_.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t off = Le32(&d[16 + i * 4]);
    if (off + 4 > d.size()) return false;
    uint32_t w = Le32(&d[off]);
    if (w > 64 || off + 4 + size_t(w) * height > d.size()) return false;
    Glyph& g = glyphs_[i];
    g.width = int(w);
    g.ink.resize(size_t(w) * height);
    for (size_t k = 0; k < g.ink.size(); ++k) g.ink[k] = d[off + 4 + k] != uint8_t(transparent);
  }
  return true;
}

int FntFont::GlyphWidth(unsigned char c) const {
  if (c >= glyphs_.size()) c = '?';
  return c < glyphs_.size() ? glyphs_[c].width : 0;
}

int FntFont::TextWidth(const std::string& s, int spacing) const {
  int w = 0;
  for (unsigned char c : s) w += GlyphWidth(c) + spacing;
  return w > 0 ? w - spacing : 0;
}

}  // namespace asc
