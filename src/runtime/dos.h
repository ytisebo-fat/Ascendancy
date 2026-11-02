#pragma once
// Emulation of the DOS / DOS-extender / PC-hardware services the original
// game code expects. The game code runs natively; whenever it executes an
// instruction that is not allowed in a Windows user-mode process (INT nn,
// IN/OUT, CLI/STI, loading a DOS selector) the CPU raises an exception, which
// the vectored exception handler installed here decodes and emulates, then
// resumes execution after the instruction.
#include <windows.h>

#include <cstdint>
#include <string>

namespace asc {

class GameImage;

struct DosConfig {
  std::string gameDir;      // directory holding ASCEND.EXE and the .COB files
  bool traceInts = false;   // log every emulated interrupt
  bool traceIrq = false;
  bool allowDosMemory = false;  // debug: hand out real-mode memory (DPMI 0100h)    // log timer interrupt delivery
  int stepAfterTrap = 0;    // debug: single-step this many instructions after each trap
};

// Installs the exception handler. Must be called once before running game code.
void DosInstall(GameImage* image, const DosConfig& cfg);

// Log to ascendancy.log (and stderr). printf-style.
void Log(const char* fmt, ...);

// Registers of the interrupted game code, as seen by the emulation.
struct Regs {
  CONTEXT* c;
  uint32_t& eax() { return reinterpret_cast<uint32_t&>(c->Eax); }
  uint32_t& ebx() { return reinterpret_cast<uint32_t&>(c->Ebx); }
  uint32_t& ecx() { return reinterpret_cast<uint32_t&>(c->Ecx); }
  uint32_t& edx() { return reinterpret_cast<uint32_t&>(c->Edx); }
  uint32_t& esi() { return reinterpret_cast<uint32_t&>(c->Esi); }
  uint32_t& edi() { return reinterpret_cast<uint32_t&>(c->Edi); }
  uint16_t ax() { return uint16_t(c->Eax); }
  uint8_t al() { return uint8_t(c->Eax); }
  uint8_t ah() { return uint8_t(c->Eax >> 8); }
  uint16_t bx() { return uint16_t(c->Ebx); }
  uint16_t cx() { return uint16_t(c->Ecx); }
  uint16_t dx() { return uint16_t(c->Edx); }
  uint8_t bl() { return uint8_t(c->Ebx); }
  uint8_t dl() { return uint8_t(c->Edx); }
  void set_ax(uint16_t v) { c->Eax = (c->Eax & 0xFFFF0000u) | v; }
  void set_al(uint8_t v) { c->Eax = (c->Eax & 0xFFFFFF00u) | v; }
  void set_ah(uint8_t v) { c->Eax = (c->Eax & 0xFFFF00FFu) | (uint32_t(v) << 8); }
  void set_bx(uint16_t v) { c->Ebx = (c->Ebx & 0xFFFF0000u) | v; }
  void set_cx(uint16_t v) { c->Ecx = (c->Ecx & 0xFFFF0000u) | v; }
  void set_dx(uint16_t v) { c->Edx = (c->Edx & 0xFFFF0000u) | v; }
  void set_si(uint16_t v) { c->Esi = (c->Esi & 0xFFFF0000u) | v; }
  void set_di(uint16_t v) { c->Edi = (c->Edi & 0xFFFF0000u) | v; }
  void cf(bool on) { if (on) c->EFlags |= 1; else c->EFlags &= ~1u; }
  void zf(bool on) { if (on) c->EFlags |= 0x40; else c->EFlags &= ~0x40u; }
  // DOS error return: CF=1, AX=code
  void fail(uint16_t code) { set_ax(code); cf(true); }
};

}  // namespace asc
