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
    // Instruction "mov dword [ebp+d8], imm32" (C7 45 d8 imm32) whose imm32 is
    // a real-mode low-memory address (BIOS data area, e.g. 0x4F0): the
    // immediate is redirected into the port's low-memory shadow.
    kLowMemImm,
    // Overwrite with `repl` bytes (e.g. a whole function body).
    kReplace,
    // "mov esp, imm32" (BC imm32) switching to a tiny private interrupt stack
    // inside the data segment. On Windows, exceptions raised while on that
    // stack (emulated instructions) push ~1-2 KB of context and would
    // overwrite neighbouring data, so it is redirected to a 256 KB stack.
    kIrqStack,
  };
  uint32_t orig;      // original address
  uint8_t bytes[8];   // expected original bytes
  uint8_t len;
  Kind kind;
  uint8_t checkLen = 0;  // bytes to verify (0 = len); less when operands are relocated
  uint8_t repl[8] = {};  // kReplace: new bytes (len of them)
  const char* note = "";
};

// Shadow of real-mode low memory 0x000-0x4FF (BIOS data area). Windows never
// maps the first 64 KB, so code that addresses it directly is redirected here.
uint8_t* LowMem();

// Applies all patches; returns false (and logs) if the bytes do not match.
bool ApplyPatches(GameImage& image);

// For the exception handler: if hostEip is a UD2-marked patch site, performs
// the emulated action on the context and returns the bytes to skip; else 0.
int EmulatePatchedSite(uint32_t hostEip, uint32_t* esp);

}  // namespace asc
