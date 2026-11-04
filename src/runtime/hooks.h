#pragma once
// Function-level hooks: the entry of an original function is overwritten with
// a jump to a C++ replacement. This is how the port swaps out whole
// subsystems (video driver, later sound, input, and game logic such as the AI).
//
// The replacement must use the same calling convention as the original:
//   - functions the game declares cdecl (e.g. the Miles VFX/AIL libraries):
//     a plain __cdecl C++ function works.
//   - Watcom register-convention functions need an adapter (WATCALL_THUNK).
#include <cstdint>

namespace asc {

class GameImage;

struct HookSpec {
  const char* name;
  uint32_t orig;           // original address of the function
  uint8_t expect[8];       // expected first bytes (verified before patching)
  uint8_t expectLen;
  void* replacement;
};

// Checks the original bytes only (to install a group of hooks all-or-nothing).
bool VerifyHook(GameImage& image, const HookSpec& spec);

// Verifies and installs; logs and returns false on mismatch.
bool InstallHook(GameImage& image, const HookSpec& spec);

}  // namespace asc
