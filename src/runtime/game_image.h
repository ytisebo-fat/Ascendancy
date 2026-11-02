#pragma once
// The original game executable, mapped into this (32-bit) process so its
// code can run natively on the CPU.
#include <cstdint>
#include <string>
#include <vector>

#include "le/le_image.h"

namespace asc {

// Known addresses in ASCEND.EXE (Dec 1996 build), at the LE preferred base.
// These are "original" addresses - use GameImage::Host() to translate.
namespace addr {
constexpr uint32_t kInitRtns = 0x76C76;   // void __InitRtns(int max_priority)
constexpr uint32_t kCMain = 0x76C27;      // void __CMain(void) - calls main(), exit()
constexpr uint32_t kMain = 0x1C424;       // int main(int argc, char** argv)  (corn.cpp)
constexpr uint32_t kGetDsSlot = 0x73A49;  // u16 in code: DS for __interrupt handlers

// Watcom C runtime globals normally set by _cstart_ (which we skip because it
// manipulates DOS selectors that do not exist on Windows).
constexpr uint32_t kStackTop = 0x897C4;     // _STACKTOP
constexpr uint32_t kInitialEsp2 = 0x897C8;  // initial ESP; later used by the heap code
constexpr uint32_t kLpCmdLine = 0x897CC;    // char* command line (argv parsing)
constexpr uint32_t kLpPgmName = 0x897D0;    // char* program name (argv[0])
constexpr uint32_t kPspSel = 0x897D4;       // u16 PSP selector
constexpr uint32_t kStackLow = 0x897D8;     // _STACKLOW (stackavail(), stack checks)
constexpr uint32_t kInitialEsp = 0x897DC;
constexpr uint32_t kNo87 = 0x897F4;         // u8 "NO87=" environment flag
constexpr uint32_t kExtender = 0x897F6;     // u8 1 = DOS/4GW
constexpr uint32_t kExtSubtype = 0x897F7;   // u8
constexpr uint32_t kEnvPtr = 0x897F9;       // char* environment block
constexpr uint32_t kEnvSel = 0x897FD;       // u16
constexpr uint32_t kDosMajor = 0x897FF;     // u8
constexpr uint32_t kDosMinor = 0x89800;     // u8
constexpr uint32_t kBssStart = 0x8A0D4;
constexpr uint32_t kBssEnd = 0x1228E4;      // end of BSS (start of heap)
}  // namespace addr

class GameImage {
 public:
  static constexpr uint32_t kDataBias = 0x10000000;

  bool Load(const std::string& exePath, std::string* error);

  // Original (LE preferred-base) address -> address in this process.
  uint32_t Host(uint32_t orig) const;
  // Address in this process -> original address (for logs); 0 if outside.
  uint32_t Orig(uint32_t host) const;
  bool ContainsHost(uint32_t host) const { return Orig(host) != 0; }
  bool IsCodeHost(uint32_t host) const;

  template <typename T>
  T* Ptr(uint32_t orig) const { return reinterpret_cast<T*>(uintptr_t(Host(orig))); }

  const LeImage& le() const { return le_; }
  // true when placed at the standard addresses (code 1:1, data +0x10000000)
  bool atPreferredBase() const { return atPreferredBase_; }

 private:
  LeImage le_;
  std::vector<uint32_t> hostBase_;  // per object
  bool atPreferredBase_ = false;
};

}  // namespace asc
