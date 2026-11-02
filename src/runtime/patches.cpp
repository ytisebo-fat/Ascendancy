#include "runtime/patches.h"

#include <windows.h>

#include <cstring>
#include <map>

#include "runtime/dos.h"
#include "runtime/game_image.h"

namespace asc {
namespace {

// ASCEND.EXE, Dec 24 1996 build (587,475 bytes).
const Patch kPatches[] = {
    {0x6F6C4, {0x0F, 0xA9, 0x0F, 0xA1}, 4, Patch::kPopSegPair},
    {0x6F6E5, {0x0F, 0xA9, 0x0F, 0xA1}, 4, Patch::kPopSegPair},
    {0x717F7, {0x8E, 0x60, 0x08}, 3, Patch::kSkipSegLoad},
    {0x717FA, {0x8E, 0x68, 0x0A}, 3, Patch::kSkipSegLoad},
    {0x71860, {0x0F, 0xA9, 0x0F, 0xA1}, 4, Patch::kPopSegPair},
    {0x7230E, {0x0F, 0xA9, 0x0F, 0xA1}, 4, Patch::kPopSegPair},
    {0x73BCC, {0x0F, 0xA9, 0x0F, 0xA1}, 4, Patch::kPopSegPair},
    {0x77898, {0x0F, 0xA1, 0x0F, 0xA9}, 4, Patch::kPopSegPair},
    {0x778C7, {0x8E, 0x63, 0x20}, 3, Patch::kSkipSegLoad},
    {0x778CA, {0x8E, 0x6B, 0x22}, 3, Patch::kSkipSegLoad},
    {0x7C937, {0x8E, 0x25}, 6, Patch::kSkipSegLoad, 2},  // mov fs,[addr] (addr relocated)
    {0x7C949, {0x8E, 0xC0}, 2, Patch::kSkipSegLoad},  // mov es,eax (eax = env selector via fs)
    {0x7C99D, {0x8E, 0xC0}, 2, Patch::kSkipSegLoad},  // same
    {0x7C9ED, {0x0F, 0xA1}, 2, Patch::kPopSeg},
    {0x7D0D1, {0x8E, 0xE2}, 2, Patch::kSkipSegLoad},
    {0x7D0DE, {0x8E, 0xEA}, 2, Patch::kSkipSegLoad},
};

std::map<uint32_t, const Patch*> g_sites;  // host address -> patch (UD2 kinds)

}  // namespace

bool ApplyPatches(GameImage& image) {
  const auto& objs = image.le().objects();
  int applied = 0;
  for (const Patch& p : kPatches) {
    // Verify against the original, unrelocated bytes.
    const LeObject* obj = nullptr;
    for (const auto& o : objs)
      if (p.orig >= o.preferredBase && p.orig + p.len <= o.preferredBase + o.virtualSize) obj = &o;
    if (!obj || std::memcmp(&obj->data[p.orig - obj->preferredBase], p.bytes, p.checkLen ? p.checkLen : p.len) != 0) {
      Log("patch at %08x: unexpected bytes - unsupported ASCEND.EXE version?", p.orig);
      return false;
    }
    uint8_t* host = image.Ptr<uint8_t>(p.orig);
    switch (p.kind) {
      case Patch::kPopSegPair: {
        const uint8_t lea[4] = {0x8D, 0x64, 0x24, 0x08};  // lea esp,[esp+8]
        std::memcpy(host, lea, 4);
        break;
      }
      case Patch::kPopSeg:
      case Patch::kSkipSegLoad:
        host[0] = 0x0F;  // UD2
        host[1] = 0x0B;
        g_sites[uint32_t(uintptr_t(host))] = &p;
        break;
    }
    ++applied;
  }
  FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
  Log("applied %d code patches", applied);
  return true;
}

int EmulatePatchedSite(uint32_t hostEip, uint32_t* esp) {
  auto it = g_sites.find(hostEip);
  if (it == g_sites.end()) return 0;
  if (it->second->kind == Patch::kPopSeg) *esp += 4;
  return it->second->len;
}

}  // namespace asc
