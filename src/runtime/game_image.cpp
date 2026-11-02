#include "runtime/game_image.h"

#include <windows.h>

namespace asc {

bool GameImage::Load(const std::string& exePath, std::string* error) {
  if (!le_.Load(exePath, error)) return false;
  const auto& objs = le_.objects();

  // Placement: code at its original address (0x10000) so crash addresses
  // match the disassembly; data at original + 0x10000000 (0x80000 ->
  // 0x10080000). The data object's preferred range would overlap the DOS
  // conventional-memory / VGA area (0x80000-0xBFFFF), which the emulation
  // keeps free for DOS memory blocks and the 0xA0000 video window - under
  // the real DOS/4GW the objects were never at these addresses either.
  // Falls back to any address (every reference is relocated).
  std::vector<void*> mem(objs.size(), nullptr);
  atPreferredBase_ = true;
  for (size_t i = 0; i < objs.size(); ++i) {
    uint32_t want = objs[i].preferredBase + (objs[i].executable() ? 0 : kDataBias);
    mem[i] = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(want)), objs[i].virtualSize,
                          MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem[i]) {
      atPreferredBase_ = false;
      mem[i] = VirtualAlloc(nullptr, objs[i].virtualSize, MEM_RESERVE | MEM_COMMIT,
                            PAGE_EXECUTE_READWRITE);
    }
    if (!mem[i]) {
      if (error) *error = "out of memory mapping the game image";
      return false;
    }
  }

  hostBase_.clear();
  std::vector<uint8_t*> dest;
  for (void* m : mem) {
    hostBase_.push_back(uint32_t(uintptr_t(m)));
    dest.push_back(static_cast<uint8_t*>(m));
  }
  uint16_t cs, ds;
  __asm__ volatile("mov %%cs, %0" : "=r"(cs));
  __asm__ volatile("mov %%ds, %0" : "=r"(ds));
  if (!le_.Relocate(dest, hostBase_, cs, ds, error)) return false;
  FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
  return true;
}

uint32_t GameImage::Host(uint32_t orig) const {
  const auto& objs = le_.objects();
  for (size_t i = 0; i < objs.size(); ++i) {
    uint32_t b = objs[i].preferredBase;
    // allow one-past-end (end-of-BSS pointers) and a little slack
    if (orig >= b && orig <= b + objs[i].virtualSize) return hostBase_[i] + (orig - b);
  }
  return orig;  // not in the image (e.g. 0xA0000) - unchanged
}

uint32_t GameImage::Orig(uint32_t host) const {
  const auto& objs = le_.objects();
  for (size_t i = 0; i < objs.size(); ++i)
    if (host >= hostBase_[i] && host < hostBase_[i] + objs[i].virtualSize)
      return objs[i].preferredBase + (host - hostBase_[i]);
  return 0;
}

bool GameImage::IsCodeHost(uint32_t host) const {
  const auto& objs = le_.objects();
  for (size_t i = 0; i < objs.size(); ++i)
    if (objs[i].executable() && host >= hostBase_[i] && host < hostBase_[i] + objs[i].virtualSize)
      return true;
  return false;
}

}  // namespace asc
