#pragma once
// Load-time patches to the original code. Each patch names an address in the
// original executable and the exact bytes expected there (checked against the
// unrelocated file, so a different game version is detected, not corrupted).
#include <cstdint>

namespace asc {

class GameImage;

struct Patch {
  enum Kind : uint8_t {
    // "pop gs; pop fs" (or reverse): replaced by "lea esp,[esp+8]" - same
    // stack effect, but no segment reload. Reloading FS (Windows' thread
    // information block) breaks exception handling under Wine and is never
    // needed: the game runs in a flat 32-bit model.
    kPopSegPair,
    // single "pop fs"/"pop gs": replaced by UD2, emulated as ESP += 4
    kPopSeg,
    // "mov fs/gs, ...": replaced by UD2, emulated as a no-op
    kSkipSegLoad,
  };
  uint32_t orig;      // original address
  uint8_t bytes[8];   // expected original bytes
  uint8_t len;
  Kind kind;
  uint8_t checkLen = 0;  // bytes to verify (0 = len); less when operands are relocated
};

// Applies all patches; returns false (and logs) if the bytes do not match.
bool ApplyPatches(GameImage& image);

// For the exception handler: if hostEip is a UD2-marked patch site, performs
// the emulated action on the context and returns the bytes to skip; else 0.
int EmulatePatchedSite(uint32_t hostEip, uint32_t* esp);

}  // namespace asc
