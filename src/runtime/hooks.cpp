#include "runtime/hooks.h"

#include <windows.h>

#include <cstring>

#include "runtime/dos.h"
#include "runtime/game_image.h"

namespace asc {

bool VerifyHook(GameImage& image, const HookSpec& spec) {
  const LeObject* obj = nullptr;
  for (const auto& o : image.le().objects())
    if (spec.orig >= o.preferredBase && spec.orig + 5 <= o.preferredBase + o.virtualSize) obj = &o;
  if (!obj || std::memcmp(&obj->data[spec.orig - obj->preferredBase], spec.expect, spec.expectLen) != 0) {
    Log("hook %s at %08x: unexpected bytes - unsupported ASCEND.EXE version?", spec.name, spec.orig);
    return false;
  }
  return true;
}

bool InstallHook(GameImage& image, const HookSpec& spec) {
  const LeObject* obj = nullptr;
  for (const auto& o : image.le().objects())
    if (spec.orig >= o.preferredBase && spec.orig + 5 <= o.preferredBase + o.virtualSize) obj = &o;
  if (!obj || std::memcmp(&obj->data[spec.orig - obj->preferredBase], spec.expect, spec.expectLen) != 0) {
    Log("hook %s at %08x: unexpected bytes - unsupported ASCEND.EXE version?", spec.name, spec.orig);
    return false;
  }
  uint8_t* p = image.Ptr<uint8_t>(spec.orig);
  int32_t rel = int32_t(uintptr_t(spec.replacement) - (uintptr_t(p) + 5));
  p[0] = 0xE9;  // jmp rel32
  std::memcpy(p + 1, &rel, 4);
  FlushInstructionCache(GetCurrentProcess(), p, 5);
  return true;
}

}  // namespace asc
