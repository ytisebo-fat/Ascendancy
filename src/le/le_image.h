#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace asc {

// Linear Executable (LE) image as produced by Watcom for DOS/4GW.
// ascend.exe = MZ stub + LE header; 2 objects (code, data), flat 32-bit model.
struct LeObject {
  uint32_t virtualSize = 0;
  uint32_t preferredBase = 0;   // e.g. 0x10000 code, 0x80000 data
  uint32_t flags = 0;           // 0x0004 executable, 0x0002 writable, 0x2000 32-bit
  uint32_t pageIndex = 0;       // 1-based index into the object page table
  uint32_t pageCount = 0;
  std::vector<uint8_t> data;    // raw contents, zero-filled to virtualSize
  bool executable() const { return flags & 0x0004; }
};

enum class LeFixupType : uint8_t {
  Selector16 = 0x02,    // 16-bit selector of the target object
  Ptr1616 = 0x03,
  Offset16 = 0x05,
  Ptr1632 = 0x06,       // 32-bit offset + 16-bit selector
  Offset32 = 0x07,      // absolute 32-bit address of target
  Relative32 = 0x08,    // target - (source + 4)
};

struct LeFixup {
  uint32_t srcObject;   // 0-based
  uint32_t srcOffset;   // object-relative
  LeFixupType type;
  uint32_t dstObject;   // 0-based
  uint32_t dstOffset;   // object-relative
};

class LeImage {
 public:
  bool Load(const std::string& path, std::string* error = nullptr);
  bool Parse(const std::vector<uint8_t>& file, std::string* error = nullptr);

  const std::vector<LeObject>& objects() const { return objects_; }
  const std::vector<LeFixup>& fixups() const { return fixups_; }
  uint32_t entryObject() const { return eipObject_; }    // 0-based
  uint32_t entryOffset() const { return eip_; }
  uint32_t stackObject() const { return espObject_; }    // 0-based
  uint32_t stackOffset() const { return esp_; }

  // Copies every object to `dest[i]` (which must hold virtualSize bytes) and
  // applies all fixups as if object i lived at linear address `base[i]`.
  // Selector fixups get codeSel for executable objects, dataSel otherwise.
  // dest[i] may equal base[i] when loading in place.
  bool Relocate(const std::vector<uint8_t*>& dest, const std::vector<uint32_t>& base,
                uint16_t codeSel, uint16_t dataSel, std::string* error = nullptr) const;

  // Convenience: relocated copies at the preferred bases (for analysis tools).
  std::vector<std::vector<uint8_t>> RelocatedAtPreferredBase() const;

 private:
  std::vector<LeObject> objects_;
  std::vector<LeFixup> fixups_;
  uint32_t eipObject_ = 0, eip_ = 0, espObject_ = 0, esp_ = 0;
};

}  // namespace asc
