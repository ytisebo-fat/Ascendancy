#include "le/le_image.h"

#include <cstdio>
#include <cstring>

namespace asc {
namespace {

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void Put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
bool Fail(std::string* e, const std::string& m) {
  if (e) *e = m;
  return false;
}

}  // namespace

bool LeImage::Load(const std::string& path, std::string* error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return Fail(error, "cannot open " + path);
  std::vector<uint8_t> buf;
  std::fseek(f, 0, SEEK_END);
  buf.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  bool ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
  std::fclose(f);
  if (!ok) return Fail(error, "cannot read " + path);
  return Parse(buf, error);
}

bool LeImage::Parse(const std::vector<uint8_t>& d, std::string* error) {
  objects_.clear();
  fixups_.clear();
  if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return Fail(error, "not an MZ executable");
  uint32_t le = Le32(&d[0x3C]);
  if (le + 0xAC > d.size() || d[le] != 'L' || d[le + 1] != 'E')
    return Fail(error, "no LE header (is this the DOS/4GW game executable?)");
  const uint8_t* h = &d[le];
  uint32_t numPages = Le32(h + 0x14);
  eipObject_ = Le32(h + 0x18) - 1;
  eip_ = Le32(h + 0x1C);
  espObject_ = Le32(h + 0x20) - 1;
  esp_ = Le32(h + 0x24);
  uint32_t pageSize = Le32(h + 0x28);
  uint32_t lastPageSize = Le32(h + 0x2C);
  uint32_t objTable = le + Le32(h + 0x40);
  uint32_t objCount = Le32(h + 0x44);
  uint32_t pageMap = le + Le32(h + 0x48);
  uint32_t fixPageTable = le + Le32(h + 0x68);
  uint32_t fixRecTable = le + Le32(h + 0x6C);
  uint32_t dataPages = Le32(h + 0x80);  // absolute file offset
  if (objCount == 0 || objCount > 16 || pageSize == 0)
    return Fail(error, "implausible LE header");

  // Page table: 4 bytes per page, 24-bit big-endian page number + flags.
  auto pageFileOffset = [&](uint32_t idx1) -> uint32_t {
    const uint8_t* e = &d[pageMap + (idx1 - 1) * 4];
    uint32_t num = uint32_t(e[0]) << 16 | uint32_t(e[1]) << 8 | e[2];
    return dataPages + (num - 1) * pageSize;
  };

  for (uint32_t i = 0; i < objCount; ++i) {
    const uint8_t* o = &d[objTable + i * 24];
    LeObject obj;
    obj.virtualSize = Le32(o);
    obj.preferredBase = Le32(o + 4);
    obj.flags = Le32(o + 8);
    obj.pageIndex = Le32(o + 12);
    obj.pageCount = Le32(o + 16);
    obj.data.assign(obj.virtualSize, 0);
    for (uint32_t p = 0; p < obj.pageCount; ++p) {
      uint32_t idx = obj.pageIndex + p;
      uint32_t sz = (idx == numPages) ? lastPageSize : pageSize;
      uint32_t src = pageFileOffset(idx);
      uint32_t dst = p * pageSize;
      if (dst >= obj.virtualSize) break;
      if (dst + sz > obj.virtualSize) sz = obj.virtualSize - dst;
      if (src + sz > d.size()) return Fail(error, "page data past end of file");
      std::memcpy(&obj.data[dst], &d[src], sz);
    }
    objects_.push_back(std::move(obj));
  }

  // Fixups, grouped by logical page.
  for (uint32_t oi = 0; oi < objCount; ++oi) {
    const LeObject& obj = objects_[oi];
    for (uint32_t p = 0; p < obj.pageCount; ++p) {
      uint32_t idx = obj.pageIndex + p;  // 1-based
      uint32_t start = fixRecTable + Le32(&d[fixPageTable + (idx - 1) * 4]);
      uint32_t end = fixRecTable + Le32(&d[fixPageTable + idx * 4]);
      uint32_t pageBase = p * pageSize;
      uint32_t q = start;
      while (q < end) {
        uint8_t src = d[q++];
        uint8_t flg = d[q++];
        std::vector<int16_t> srcOffs;
        uint8_t listCount = 0;
        if (src & 0x20) listCount = d[q++];
        else { srcOffs.push_back(int16_t(Le16(&d[q]))); q += 2; }
        if ((flg & 3) != 0)
          return Fail(error, "imported/entry fixups are not supported (unexpected for DOS/4GW)");
        uint32_t tobj;
        if (flg & 0x40) { tobj = Le16(&d[q]); q += 2; } else { tobj = d[q++]; }
        uint32_t toff = 0;
        uint8_t stype = src & 0x0F;
        if (stype != 0x02) {
          if (flg & 0x10) { toff = Le32(&d[q]); q += 4; } else { toff = Le16(&d[q]); q += 2; }
        }
        if (flg & 0x04) {  // additive
          uint32_t add;
          if (flg & 0x20) { add = Le32(&d[q]); q += 4; } else { add = Le16(&d[q]); q += 2; }
          toff += add;
        }
        for (uint8_t k = 0; k < listCount; ++k) { srcOffs.push_back(int16_t(Le16(&d[q]))); q += 2; }
        if (tobj == 0 || tobj > objCount) return Fail(error, "fixup targets bad object");
        for (int16_t so : srcOffs) {
          int64_t off = int64_t(pageBase) + so;
          if (off < 0 || off >= int64_t(obj.virtualSize)) continue;
          fixups_.push_back({oi, uint32_t(off), LeFixupType(stype), tobj - 1, toff});
        }
      }
    }
  }
  return true;
}

bool LeImage::Relocate(const std::vector<uint8_t*>& dest, const std::vector<uint32_t>& base,
                       uint16_t codeSel, uint16_t dataSel, std::string* error) const {
  if (dest.size() != objects_.size() || base.size() != objects_.size())
    return Fail(error, "Relocate: wrong number of objects");
  for (size_t i = 0; i < objects_.size(); ++i)
    std::memcpy(dest[i], objects_[i].data.data(), objects_[i].virtualSize);
  for (const LeFixup& f : fixups_) {
    const LeObject& so = objects_[f.srcObject];
    uint8_t* p = dest[f.srcObject] + f.srcOffset;
    uint32_t target = base[f.dstObject] + f.dstOffset;
    uint16_t sel = objects_[f.dstObject].executable() ? codeSel : dataSel;
    auto room = [&](uint32_t n) { return f.srcOffset + n <= so.virtualSize; };
    switch (f.type) {
      case LeFixupType::Offset32:
        if (room(4)) Put32(p, target);
        break;
      case LeFixupType::Relative32:
        if (room(4)) Put32(p, target - (base[f.srcObject] + f.srcOffset + 4));
        break;
      case LeFixupType::Selector16:
        if (room(2)) Put16(p, sel);
        break;
      case LeFixupType::Ptr1632:
        if (room(6)) { Put32(p, target); Put16(p + 4, sel); }
        break;
      case LeFixupType::Offset16:
        if (room(2)) Put16(p, uint16_t(target));
        break;
      case LeFixupType::Ptr1616:
        if (room(4)) { Put16(p, uint16_t(target)); Put16(p + 2, sel); }
        break;
      default:
        return Fail(error, "unsupported fixup type");
    }
  }
  return true;
}

std::vector<std::vector<uint8_t>> LeImage::RelocatedAtPreferredBase() const {
  std::vector<std::vector<uint8_t>> out(objects_.size());
  std::vector<uint8_t*> dest;
  std::vector<uint32_t> base;
  for (size_t i = 0; i < objects_.size(); ++i) {
    out[i].resize(objects_[i].virtualSize);
    dest.push_back(out[i].data());
    base.push_back(objects_[i].preferredBase);
  }
  // Selector values are placeholders that are easy to spot in a disassembly.
  Relocate(dest, base, 0xC0DE, 0xDA7A);
  return out;
}

}  // namespace asc
