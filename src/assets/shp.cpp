#include "assets/shp.h"

#include <cstring>

namespace asc {
namespace {

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool DecodeFrame(const std::vector<uint8_t>& d, uint32_t off, ShpFrame* f) {
  if (off + 24 > d.size()) return false;
  int32_t x0 = int32_t(Le32(&d[off + 8])), y0 = int32_t(Le32(&d[off + 12]));
  int32_t x1 = int32_t(Le32(&d[off + 16])), y1 = int32_t(Le32(&d[off + 20]));
  auto sentinel = [](int32_t v) { return v > 0x7FFF0000; };
  if (x0 > x1 || y0 > y1 || sentinel(x0) || sentinel(x1) || sentinel(y0) || sentinel(y1))
    return true;  // empty frame
  int w = x1 - x0 + 1, h = y1 - y0 + 1;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
  f->width = w;
  f->height = h;
  f->xMin = x0;
  f->yMin = y0;
  f->pixels.assign(size_t(w) * h, 0);
  f->mask.assign(size_t(w) * h, 0);

  size_t p = off + 24;
  int x = 0, y = 0;
  while (y < h) {
    if (p >= d.size()) return false;
    uint8_t b = d[p++];
    if (b == 0) {
      x = 0;
      ++y;
    } else if (b == 1) {
      if (p >= d.size()) return false;
      x += d[p++];
    } else {
      int n = b >> 1;
      if (x + n > w) return false;
      uint8_t* dst = &f->pixels[size_t(y) * w + x];
      if (b & 1) {
        if (p + n > d.size()) return false;
        std::memcpy(dst, &d[p], n);
        p += n;
      } else {
        if (p >= d.size()) return false;
        std::memset(dst, d[p++], n);
      }
      std::memset(&f->mask[size_t(y) * w + x], 1, n);
      x += n;
    }
  }
  return true;
}

}  // namespace

bool ShpFile::Parse(const std::vector<uint8_t>& d) {
  frames_.clear();
  palettes_.clear();
  if (d.size() < 8 || std::memcmp(d.data(), "1.10", 4) != 0) return false;
  uint32_t count = Le32(&d[4]);
  if (count > 10000 || 8 + size_t(count) * 8 > d.size()) return false;
  frames_.resize(count);
  palettes_.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t off = Le32(&d[8 + i * 8]);
    palettes_[i] = Le32(&d[12 + i * 8]);
    if (!DecodeFrame(d, off, &frames_[i])) return false;
  }
  return true;
}

std::vector<uint8_t> ShpFile::ToRGBA(const ShpFrame& f, const Palette& pal) {
  std::vector<uint8_t> out(size_t(f.width) * f.height * 4, 0);
  for (size_t i = 0; i < f.pixels.size(); ++i) {
    if (!f.mask[i]) continue;
    uint32_t c = pal.rgba[f.pixels[i]];
    std::memcpy(&out[i * 4], &c, 4);
  }
  return out;
}

}  // namespace asc
