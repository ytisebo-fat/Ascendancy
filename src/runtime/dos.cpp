#include "runtime/dos.h"

#include <cstdarg>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "runtime/game_image.h"
#include "runtime/patches.h"
#include "runtime/video.h"

namespace asc {
namespace {

GameImage* g_image = nullptr;
DosConfig g_cfg;
FILE* g_log = nullptr;
uint16_t g_flatDs = 0;

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
struct DosFile {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool device = false;
};
std::vector<DosFile> g_files;  // index = DOS handle

struct FindState {
  HANDLE h = INVALID_HANDLE_VALUE;
  std::string dir;
  uint8_t attrMask = 0;
};
std::map<uint32_t, FindState> g_finds;  // key = DTA address
uint32_t g_dta = 0;
uint32_t g_nextFindId = 1;

// ---------------------------------------------------------------------------
// Interrupt vectors installed by the game (protected mode, DPMI 0205 / 21h 25h)
// ---------------------------------------------------------------------------
struct Vector { uint16_t sel = 0; uint32_t off = 0; };
Vector g_pmVectors[256];
uint32_t g_rmVectors[256];

// Pseudo selectors handed out by DPMI 0000h (cannot create real LDT entries on
// Windows); remembered so we can report what the game tried to do with them.
struct PseudoSel { uint32_t base = 0, limit = 0; };
std::map<uint16_t, PseudoSel> g_selectors;
uint16_t g_nextSel = 0x1007;

// DOS conventional memory arena (DPMI 0100h). Kept below 1 MB so code that
// computes "segment << 4" reaches the block, as under DOS/4GW.
constexpr uint32_t kDosArenaBase = 0x80000, kDosArenaSize = 0x20000;
uint32_t g_dosArenaNext = 0;
bool g_dosArenaOk = false;

volatile bool g_if = true;  // virtual interrupt flag (CLI/STI)

// --- Hardware interrupt delivery (IRQ0 timer) ------------------------------
HANDLE g_gameThread = nullptr;
volatile LONG g_irqPending = 0;        // timer ticks waiting to be delivered
volatile LONG g_pitDivisor = 65536;    // 1193182 Hz / divisor = tick rate
bool g_pitHighNext = false;
uint16_t g_pitLow = 0;
uint8_t* g_iretStub = nullptr;         // "old handler" for unset vectors
uint16_t g_codeSel = 0;
int g_stepBudget = 0;
volatile uint32_t g_watchAddr = 0;  // debug: hardware write watchpoint (orig offscreen pixel)
bool g_watchArmed = false;  // --step N: single-step N instructions after each trap

}  // namespace

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (!g_log) g_log = std::fopen("ascendancy.log", "w");
  if (g_log) { std::fputs(buf, g_log); std::fputc('\n', g_log); std::fflush(g_log); }
  std::fputs(buf, stderr);
  std::fputc('\n', stderr);
}

namespace {

[[noreturn]] void Fatal(CONTEXT* c, const char* what) {
  uint32_t eip = c->Eip;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(uintptr_t(eip));
  Log("FATAL: %s at EIP=%08x (orig %08x)", what, eip, g_image ? g_image->Orig(eip) : 0);
  Log("  bytes: %02x %02x %02x %02x %02x %02x %02x %02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
  Log("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx",
      c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
  const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(c->Esp));
  for (int i = 0; i < 16; ++i) {
    uint32_t o = g_image ? g_image->Orig(sp[i]) : 0;
    if (o) Log("  stack[%2d] %08x  (orig %08x)", i, sp[i], o);
  }
  if (g_log) std::fclose(g_log);
  char msg[256];
  std::snprintf(msg, sizeof msg,
                "The game hit something the port does not handle yet:\n%s at %08x.\n\n"
                "Details are in ascendancy.log.", what, g_image ? g_image->Orig(eip) : eip);
  MessageBoxA(nullptr, msg, "Ascendancy", MB_ICONERROR);
  ExitProcess(3);
}

const char* Str(uint32_t p) { return reinterpret_cast<const char*>(uintptr_t(p)); }
uint8_t* Mem(uint32_t p) { return reinterpret_cast<uint8_t*>(uintptr_t(p)); }

std::string HostPath(const char* dosPath) {
  std::string p(dosPath);
  // Absolute DOS paths ("C:\ASCEND\x") are taken relative to the game dir.
  if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
  for (char& ch : p) if (ch == '/') ch = '\\';
  if (!p.empty() && p[0] == '\\') {
    // strip any leading directories; the game only uses its own directory tree
    size_t pos = p.find_last_of('\\');
    std::string tail = p.substr(pos + 1);
    p = tail;
  }
  return g_cfg.gameDir + "\\" + p;
}

uint16_t DosErr(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND: return 2;
    case ERROR_PATH_NOT_FOUND: return 3;
    case ERROR_TOO_MANY_OPEN_FILES: return 4;
    case ERROR_ACCESS_DENIED: return 5;
    case ERROR_INVALID_HANDLE: return 6;
    case ERROR_NO_MORE_FILES: return 18;
    default: return 5;
  }
}

int NewHandle(HANDLE h) {
  for (size_t i = 5; i < g_files.size(); ++i)
    if (g_files[i].h == INVALID_HANDLE_VALUE) { g_files[i].h = h; g_files[i].device = false; return int(i); }
  g_files.push_back({h, false});
  return int(g_files.size() - 1);
}

DosFile* GetFile(uint16_t handle) {
  if (handle >= g_files.size() || g_files[handle].h == INVALID_HANDLE_VALUE) return nullptr;
  return &g_files[handle];
}

void FillDta(const WIN32_FIND_DATAA& fd) {
  uint8_t* dta = Mem(g_dta);
  FILETIME local;
  WORD date = 0, time = 0;
  FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local);
  FileTimeToDosDateTime(&local, &date, &time);
  uint8_t attr = uint8_t(fd.dwFileAttributes & 0x37);
  dta[0x15] = attr;
  std::memcpy(dta + 0x16, &time, 2);
  std::memcpy(dta + 0x18, &date, 2);
  uint32_t size = fd.nFileSizeLow;
  std::memcpy(dta + 0x1A, &size, 4);
  const char* name = fd.cAlternateFileName[0] ? fd.cAlternateFileName : fd.cFileName;
  char upper[13] = {};
  for (int i = 0; i < 12 && name[i]; ++i) upper[i] = char(toupper((unsigned char)name[i]));
  std::memcpy(dta + 0x1E, upper, 13);
}

bool FindMatches(const WIN32_FIND_DATAA& fd, uint8_t mask) {
  uint8_t a = uint8_t(fd.dwFileAttributes & 0x37);
  // hidden/system/directory entries only when requested
  return (a & 0x16 & ~mask) == 0;
}

// ---------------------------------------------------------------------------
// INT 21h  (DOS, with DOS/4GW's 32-bit register extensions)
// ---------------------------------------------------------------------------
void Int21(Regs& r) {
  r.cf(false);
  switch (r.ah()) {
    case 0x02: std::fputc(r.dl(), stdout); break;
    case 0x06:  // direct console I/O
      if (r.dl() != 0xFF) std::fputc(r.dl(), stdout);
      else { r.set_al(0); r.zf(true); }
      break;
    case 0x09: {  // print '$'-terminated string at DS:EDX
      for (const char* s = Str(r.edx()); *s != '$'; ++s) std::fputc(*s, stdout);
      break;
    }
    case 0x0E: r.set_al(26); break;          // select drive -> 26 drives
    case 0x19: r.set_al(2); break;           // current drive = C:
    case 0x1A: g_dta = r.edx(); break;       // set DTA
    case 0x2F: r.ebx() = g_dta; break;       // get DTA
    case 0x25: {                             // set interrupt vector
      g_pmVectors[r.al()] = {g_codeSel, r.edx()};
      Log("int21 set vector %02x -> %08x (orig %08x)", r.al(), r.edx(), g_image->Orig(r.edx()));
      break;
    }
    case 0x35: {  // get interrupt vector -> ES:EBX
      const Vector& v = g_pmVectors[r.al()];
      r.ebx() = v.off ? v.off : uint32_t(uintptr_t(g_iretStub));
      r.c->SegEs = v.off ? v.sel : g_codeSel;
      break;
    }
    case 0x2A: {  // get date
      SYSTEMTIME st; GetLocalTime(&st);
      r.set_cx(st.wYear); r.set_dx(uint16_t(st.wMonth << 8 | st.wDay)); r.set_al(uint8_t(st.wDayOfWeek));
      break;
    }
    case 0x2C: {  // get time (games poll this in wait loops: keep the window alive)
      video::Present();
      SYSTEMTIME st; GetLocalTime(&st);
      r.set_cx(uint16_t(st.wHour << 8 | st.wMinute));
      r.set_dx(uint16_t(st.wSecond << 8 | (st.wMilliseconds / 10)));
      break;
    }
    case 0x30:  // DOS version 6.22
      r.set_ax(0x1606);
      r.set_bx(0);
      break;
    case 0x33:  // ctrl-break state
      if (r.al() == 0) r.c->Edx &= ~0xFFu;
      break;
    case 0x3B:  // chdir - accepted, paths are resolved against the game dir
      break;
    case 0x3C: case 0x5B: {  // create
      std::string p = HostPath(Str(r.edx()));
      HANDLE h = CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             r.ah() == 0x5B ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) { r.fail(DosErr(GetLastError())); break; }
      r.eax() = uint32_t(NewHandle(h));
      if (g_cfg.traceInts) Log("  create %s -> %u", p.c_str(), r.eax());
      break;
    }
    case 0x3D: {  // open
      std::string p = HostPath(Str(r.edx()));
      DWORD access = (r.al() & 3) == 0 ? GENERIC_READ : (r.al() & 3) == 1 ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
      HANDLE h = CreateFileA(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        if (g_cfg.traceInts) Log("  open %s FAILED", p.c_str());
        r.fail(DosErr(GetLastError()));
        break;
      }
      r.eax() = uint32_t(NewHandle(h));
      if (g_cfg.traceInts) Log("  open %s -> %u", p.c_str(), r.eax());
      break;
    }
    case 0x3E: {  // close
      DosFile* f = GetFile(r.bx());
      if (!f) { r.fail(6); break; }
      if (r.bx() >= 5) { CloseHandle(f->h); f->h = INVALID_HANDLE_VALUE; }
      break;
    }
    case 0x3F: {  // read ECX bytes to DS:EDX
      DosFile* f = GetFile(r.bx());
      if (!f) { r.fail(6); break; }
      DWORD got = 0;
      if (!ReadFile(f->h, Mem(r.edx()), r.ecx(), &got, nullptr)) { r.fail(DosErr(GetLastError())); break; }
      r.eax() = got;
      break;
    }
    case 0x40: {  // write
      DosFile* f = GetFile(r.bx());
      if (!f) { r.fail(6); break; }
      DWORD put = 0;
      if (r.ecx() == 0) {  // DOS: truncate at current position
        SetEndOfFile(f->h);
      } else if (!WriteFile(f->h, Mem(r.edx()), r.ecx(), &put, nullptr)) {
        r.fail(DosErr(GetLastError()));
        break;
      }
      r.eax() = put;
      break;
    }
    case 0x41: {  // delete
      if (!DeleteFileA(HostPath(Str(r.edx())).c_str())) r.fail(DosErr(GetLastError()));
      break;
    }
    case 0x42: {  // seek CX:DX from AL
      DosFile* f = GetFile(r.bx());
      if (!f) { r.fail(6); break; }
      LONG lo = LONG(uint32_t(r.cx()) << 16 | r.dx());
      DWORD m = r.al() == 0 ? FILE_BEGIN : r.al() == 1 ? FILE_CURRENT : FILE_END;
      DWORD pos = SetFilePointer(f->h, lo, nullptr, m);
      if (pos == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) { r.fail(25); break; }
      r.set_ax(uint16_t(pos));
      r.set_dx(uint16_t(pos >> 16));
      break;
    }
    case 0x43: {  // get/set attributes
      std::string p = HostPath(Str(r.edx()));
      DWORD a = GetFileAttributesA(p.c_str());
      if (a == INVALID_FILE_ATTRIBUTES) { r.fail(DosErr(GetLastError())); break; }
      if (r.al() == 0) r.set_cx(uint16_t(a & 0x37));
      break;
    }
    case 0x44: {  // IOCTL
      if (r.al() == 0) {  // device info
        DosFile* f = GetFile(r.bx());
        if (!f) { r.fail(6); break; }
        r.set_dx(f->device ? 0x80D3 : 0x0002);
      } else if (r.al() == 8) {
        r.set_ax(1);  // drive not removable
      } else {
        r.set_ax(0);
      }
      break;
    }
    case 0x47: {  // get current directory (DS:ESI, without drive/leading '\')
      std::strcpy(reinterpret_cast<char*>(Mem(r.esi())), "ASCEND");
      break;
    }
    case 0x4C:  // exit
      Log("game exited with code %u", r.al());
      if (g_log) std::fclose(g_log);
      ExitProcess(r.al());
    case 0x4E: case 0x4F: {  // find first / next
      if (r.ah() == 0x4E) {
        std::string pattern = HostPath(Str(r.edx()));
        FindState& fs = g_finds[g_dta];
        if (fs.h != INVALID_HANDLE_VALUE) FindClose(fs.h);
        fs.attrMask = uint8_t(r.cx());
        WIN32_FIND_DATAA fd;
        fs.h = FindFirstFileA(pattern.c_str(), &fd);
        if (fs.h == INVALID_HANDLE_VALUE) { r.fail(18); break; }
        while (!FindMatches(fd, fs.attrMask))
          if (!FindNextFileA(fs.h, &fd)) { FindClose(fs.h); fs.h = INVALID_HANDLE_VALUE; break; }
        if (fs.h == INVALID_HANDLE_VALUE) { r.fail(18); break; }
        FillDta(fd);
      } else {
        auto it = g_finds.find(g_dta);
        if (it == g_finds.end() || it->second.h == INVALID_HANDLE_VALUE) { r.fail(18); break; }
        WIN32_FIND_DATAA fd;
        bool found = false;
        while (FindNextFileA(it->second.h, &fd))
          if (FindMatches(fd, it->second.attrMask)) { found = true; break; }
        if (!found) { FindClose(it->second.h); it->second.h = INVALID_HANDLE_VALUE; r.fail(18); break; }
        FillDta(fd);
      }
      break;
    }
    case 0x56: {  // rename DS:EDX -> ES:EDI
      if (!MoveFileA(HostPath(Str(r.edx())).c_str(), HostPath(Str(r.edi())).c_str()))
        r.fail(DosErr(GetLastError()));
      break;
    }
    case 0x57: {  // file date/time
      DosFile* f = GetFile(r.bx());
      if (!f) { r.fail(6); break; }
      if (r.al() == 0) {
        FILETIME ft, local; WORD d = 0, t = 0;
        GetFileTime(f->h, nullptr, nullptr, &ft);
        FileTimeToLocalFileTime(&ft, &local);
        FileTimeToDosDateTime(&local, &d, &t);
        r.set_cx(t); r.set_dx(d);
      }
      break;
    }
    case 0x48: {  // allocate memory: EBX paragraphs (DOS/4GW extension)
      uint32_t bytes = r.ebx() * 16;
      void* m = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (!m) { r.fail(8); break; }
      r.eax() = uint32_t(uintptr_t(m));
      Log("int21 alloc %u bytes -> %p", bytes, m);
      break;
    }
    case 0x49: break;  // free: leak (rare)
    case 0x4A: break;  // resize: pretend success
    case 0xFF:         // DOS/4GW presence check (AX=FF00) - "not under DOS/4GW"
      r.set_al(0);
      break;
    default:
      Log("int21 AH=%02x (AX=%04x) not implemented", r.ah(), r.ax());
      r.fail(1);
  }
}

// ---------------------------------------------------------------------------
// INT 31h  DPMI
// ---------------------------------------------------------------------------
void Int31(Regs& r) {
  r.cf(false);
  switch (r.ax()) {
    case 0x0000: {  // allocate CX descriptors
      uint16_t first = g_nextSel;
      for (int i = 0; i < r.cx(); ++i) { g_selectors[g_nextSel] = {}; g_nextSel += 8; }
      r.set_ax(first);
      Log("dpmi alloc %u descriptors -> %04x (pseudo)", r.cx(), first);
      break;
    }
    case 0x0001: g_selectors.erase(r.bx()); break;
    case 0x0003: r.set_ax(8); break;  // selector increment
    case 0x0006: {                    // get segment base
      auto it = g_selectors.find(r.bx());
      uint32_t b = it == g_selectors.end() ? 0 : it->second.base;
      r.set_cx(uint16_t(b >> 16)); r.set_dx(uint16_t(b));
      break;
    }
    case 0x0007: g_selectors[r.bx()].base = uint32_t(r.cx()) << 16 | r.dx(); break;
    case 0x0008: g_selectors[r.bx()].limit = uint32_t(r.cx()) << 16 | r.dx(); break;
    case 0x0009: case 0x000C: break;  // access rights / set descriptor
    case 0x000A: {                    // create alias of a code selector
      uint16_t s = g_nextSel; g_nextSel += 8; g_selectors[s] = {};
      r.set_ax(s);
      break;
    }
    case 0x0100: {  // allocate DOS memory: BX paragraphs -> AX segment, DX selector
      uint32_t bytes = (uint32_t(r.bx()) * 16 + 15) & ~15u;
      // Conventional memory is only used for real-mode drivers (VESA BIOS,
      // mouse handler, Sound Blaster driver), all of which the port replaces.
      // Refusing it keeps behaviour identical on every system, and makes the
      // game skip real-mode sound drivers cleanly (no sound until the native
      // audio layer lands).
      if (!g_cfg.allowDosMemory || !g_dosArenaOk || g_dosArenaNext + bytes > kDosArenaSize) {
        r.set_bx(uint16_t((kDosArenaSize - g_dosArenaNext) / 16));
        r.fail(8);
        break;
      }
      uint32_t lin = kDosArenaBase + g_dosArenaNext;
      g_dosArenaNext += bytes;
      std::memset(Mem(lin), 0, bytes);
      uint16_t sel = g_nextSel; g_nextSel += 8; g_selectors[sel] = {lin, bytes - 1};
      r.set_ax(uint16_t(lin >> 4));
      r.set_dx(sel);
      Log("dpmi dos alloc %u bytes -> seg %04x (linear %05x) sel %04x", bytes, lin >> 4, lin, sel);
      break;
    }
    case 0x0101: break;  // free DOS memory (arena is not reclaimed)
    case 0x0200:         // get real-mode vector
      r.set_cx(uint16_t(g_rmVectors[r.bl()] >> 16)); r.set_dx(uint16_t(g_rmVectors[r.bl()]));
      break;
    case 0x0201:
      g_rmVectors[r.bl()] = uint32_t(r.cx()) << 16 | r.dx();
      break;
    case 0x0204: {  // get PM vector -> CX:EDX
      const Vector& v = g_pmVectors[r.bl()];
      r.set_cx(v.off ? v.sel : g_codeSel);
      r.edx() = v.off ? v.off : uint32_t(uintptr_t(g_iretStub));
      break;
    }
    case 0x0205:  // set PM vector
      g_pmVectors[r.bl()] = {r.cx(), r.edx()};
      Log("dpmi set PM vector %02x -> %08x (orig %08x)", r.bl(), r.edx(), g_image->Orig(r.edx()));
      break;
    case 0x0300: case 0x0301: case 0x0302: {  // simulate real-mode interrupt/call
      const uint8_t* rm = Mem(r.edi());       // real-mode register structure at ES:EDI
      uint32_t eax; std::memcpy(&eax, rm + 0x1C, 4);
      Log("dpmi real-mode int %02x AX=%04x - not emulated yet", r.bl(), eax & 0xFFFF);
      r.fail(0x8001);
      break;
    }
    case 0x0400:  // DPMI version 0.90, 386
      r.set_ax(0x005A); r.set_bx(0x0005); r.c->Ecx = (r.c->Ecx & ~0xFFu) | 3; r.set_dx(0x0870);
      break;
    case 0x0500: {  // free memory info -> 48-byte buffer at ES:EDI
      uint32_t* info = reinterpret_cast<uint32_t*>(Mem(r.edi()));
      MEMORYSTATUSEX ms{sizeof ms};
      GlobalMemoryStatusEx(&ms);
      uint32_t avail = ms.ullAvailVirtual > 0x40000000ull ? 0x40000000u : uint32_t(ms.ullAvailVirtual);
      for (int i = 0; i < 12; ++i) info[i] = 0xFFFFFFFFu;
      info[0] = avail;              // largest free block (bytes)
      info[1] = avail / 4096;       // max unlocked pages
      info[2] = avail / 4096;       // max locked pages
      info[3] = 0x40000000u / 4096; // linear space pages
      info[4] = avail / 4096;
      info[5] = avail / 4096;
      info[6] = avail / 4096;
      info[8] = avail / 4096;
      break;
    }
    case 0x0501: {  // allocate BX:CX bytes -> BX:CX linear, SI:DI handle
      uint32_t bytes = uint32_t(r.bx()) << 16 | r.cx();
      void* m = VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (!m) { r.fail(0x8012); break; }
      uint32_t a = uint32_t(uintptr_t(m));
      r.set_bx(uint16_t(a >> 16)); r.set_cx(uint16_t(a));
      r.set_si(uint16_t(a >> 16)); r.set_di(uint16_t(a));
      if (g_cfg.traceInts) Log("  dpmi alloc %u bytes -> %08x", bytes, a);
      break;
    }
    case 0x0502:  // free SI:DI
      VirtualFree(reinterpret_cast<void*>(uintptr_t(uint32_t(r.c->Esi & 0xFFFF) << 16 | (r.c->Edi & 0xFFFF))), 0, MEM_RELEASE);
      break;
    case 0x0503: {  // resize SI:DI to BX:CX (no in-place growth: allocate + copy)
      uint32_t old = uint32_t(r.c->Esi & 0xFFFF) << 16 | (r.c->Edi & 0xFFFF);
      uint32_t bytes = uint32_t(r.bx()) << 16 | r.cx();
      MEMORY_BASIC_INFORMATION mbi{};
      VirtualQuery(reinterpret_cast<void*>(uintptr_t(old)), &mbi, sizeof mbi);
      void* m = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (!m) { r.fail(0x8012); break; }
      std::memcpy(m, reinterpret_cast<void*>(uintptr_t(old)), mbi.RegionSize < bytes ? mbi.RegionSize : bytes);
      VirtualFree(reinterpret_cast<void*>(uintptr_t(old)), 0, MEM_RELEASE);
      uint32_t a = uint32_t(uintptr_t(m));
      r.set_bx(uint16_t(a >> 16)); r.set_cx(uint16_t(a));
      r.set_si(uint16_t(a >> 16)); r.set_di(uint16_t(a));
      break;
    }
    case 0x0600: case 0x0601: case 0x0702: case 0x0703: break;  // lock/unlock: no-op
    case 0x0800: {  // physical address mapping (e.g. VESA linear frame buffer)
      uint32_t phys = uint32_t(r.bx()) << 16 | r.cx();
      Log("dpmi map physical %08x - not available", phys);
      r.fail(0x8003);
      break;
    }
    case 0x0900: r.set_al(g_if ? 1 : 0); g_if = false; break;
    case 0x0901: r.set_al(g_if ? 1 : 0); g_if = true; break;
    case 0x0902: r.set_al(g_if ? 1 : 0); break;
    case 0x0A00: r.fail(0x8001); break;  // vendor API: none
    default:
      Log("int31 AX=%04x not implemented", r.ax());
      r.fail(0x8001);
  }
}

// ---------------------------------------------------------------------------
// INT 33h mouse, INT 16h keyboard, INT 10h video, INT 2Fh multiplex
// (input/video are wired to the SDL front end in a later step)
// ---------------------------------------------------------------------------
// The game sets the mouse range to 0..5112 x 0..3832 (8 units per pixel) and
// divides by 8; ranges are tracked to convert from window pixels.
struct MouseRange { int maxX = 639, maxY = 479; };
MouseRange g_mouseRange;
int ScaleX(int px) { return px * (g_mouseRange.maxX + 1) / 640; }
int ScaleY(int py) { return py * (g_mouseRange.maxY + 1) / 480; }
int UnscaleX(int v) { return v * 640 / (g_mouseRange.maxX + 1); }
int UnscaleY(int v) { return v * 480 / (g_mouseRange.maxY + 1); }

// Mirrors what the game's real-mode mouse handler stored at 0000:04F0.
void SyncMouseToBios() {
  video::Mouse m = video::GetMouseNoPump();
  uint16_t v[3] = {uint16_t(m.buttons), uint16_t(ScaleX(m.x)), uint16_t(ScaleY(m.y))};
  std::memcpy(LowMem() + 0x4F0, v, sizeof v);
}

void Int33(Regs& r) {
  switch (r.ax()) {
    case 0x0000: r.set_ax(0xFFFF); r.set_bx(2); break;  // reset: installed, 2 buttons
    case 0x0001: case 0x0002: break;                    // show/hide cursor
    case 0x0003: {
      video::Present();
      video::Mouse m = video::GetMouse();
      r.set_bx(uint16_t(m.buttons)); r.set_cx(uint16_t(ScaleX(m.x))); r.set_dx(uint16_t(ScaleY(m.y)));
      break;
    }
    case 0x0004: video::WarpMouse(UnscaleX(r.cx()), UnscaleY(r.dx())); SyncMouseToBios(); break;
    case 0x0007: g_mouseRange.maxX = r.dx() > 0 ? r.dx() : 639; break;
    case 0x0008: g_mouseRange.maxY = r.dx() > 0 ? r.dx() : 479; break;
    case 0x000F: break;  // mickey/pixel ratio
    case 0x000B: r.set_cx(0); r.set_dx(0); break;       // motion counters
    case 0x000C: Log("mouse event handler set: mask %04x -> %08x", r.cx(), r.edx()); break;
    default: if (g_cfg.traceInts) Log("int33 AX=%04x ignored", r.ax());
  }
}

void Int16(Regs& r) {
  video::Present();
  uint16_t key;
  switch (r.ah()) {
    case 0x01: case 0x11:  // key waiting? ZF=1 none, else AX=key
      if (video::PeekKey(&key)) { r.set_ax(key); r.zf(false); }
      else r.zf(true);
      break;
    case 0x00: case 0x10:  // read key (blocking)
      while (!video::PopKey(&key)) { video::Present(); Sleep(1); }
      r.set_ax(key);
      if (g_cfg.traceInts) Log("  key %04x", key);
      break;
    case 0x02: case 0x12: r.set_al(video::ShiftFlags()); break;
    default: if (g_cfg.traceInts) Log("int16 AH=%02x ignored", r.ah());
  }
}

void Int10(Regs& r) {
  Log("int10 AX=%04x BX=%04x - video BIOS not emulated yet", r.ax(), r.bx());
}

void Int2F(Regs& r) {
  switch (r.ax()) {
    case 0x1680: Sleep(0); r.set_al(0); break;  // release time slice
    default:
      if (g_cfg.traceInts) Log("int2f AX=%04x ignored", r.ax());
  }
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------
uint8_t g_dacIndexW = 0, g_dacIndexR = 0, g_dacPhase = 0;
uint8_t g_palette[768];
uint8_t g_pitLatch = 0;

uint32_t PortIn(uint16_t port, int size) {
  switch (port) {
    case 0x3DA: {  // VGA input status: toggle vertical retrace bit
      static uint8_t v = 0;
      v ^= 0x09;
      return v;
    }
    case 0x3C9: {
      uint8_t val = g_palette[g_dacIndexR * 3 + g_dacPhase];
      if (++g_dacPhase == 3) { g_dacPhase = 0; ++g_dacIndexR; }
      return val;
    }
    case 0x40: return g_pitLatch++;
    case 0x60: return 0;     // keyboard data
    case 0x61: return 0x20;
    default:
      if (g_cfg.traceInts) Log("in  port %04x (size %d)", port, size);
      return size == 1 ? 0xFF : 0xFFFFFFFFu;
  }
}

void PortOut(uint16_t port, uint32_t val, int size) {
  switch (port) {
    case 0x3C8: g_dacIndexW = uint8_t(val); g_dacPhase = 0; break;
    case 0x3C7: g_dacIndexR = uint8_t(val); g_dacPhase = 0; break;
    case 0x3C9:
      g_palette[g_dacIndexW * 3 + g_dacPhase] = uint8_t(val & 63);
      if (++g_dacPhase == 3) { g_dacPhase = 0; ++g_dacIndexW; }
      break;
    case 0x20: case 0xA0: break;  // PIC EOI
    case 0x43:  // PIT mode/command: channel 0 load low+high byte follows
      if ((val & 0xC0) == 0) g_pitHighNext = false;
      break;
    case 0x40:
      if (!g_pitHighNext) { g_pitLow = uint8_t(val); g_pitHighNext = true; }
      else {
        uint32_t d = uint32_t(g_pitLow) | uint32_t(val & 0xFF) << 8;
        g_pitHighNext = false;
        InterlockedExchange(&g_pitDivisor, d ? LONG(d) : 65536);
        Log("timer: %u Hz", unsigned(1193182 / (d ? d : 65536)));
      }
      break;
    default:
      if (g_cfg.traceInts) Log("out port %04x <- %x (size %d)", port, val, size);
  }
}

// ---------------------------------------------------------------------------
// Instruction decoding helpers
// ---------------------------------------------------------------------------
struct Prefixes {
  int len = 0;
  bool opsize = false;  // 0x66
  bool rep = false;
};

Prefixes ReadPrefixes(const uint8_t* p) {
  Prefixes x;
  for (;;) {
    uint8_t b = p[x.len];
    if (b == 0x66) x.opsize = true;
    else if (b == 0xF3 || b == 0xF2) x.rep = true;
    else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 || b == 0x67) {}
    else break;
    ++x.len;
  }
  return x;
}

// Length of a ModRM operand (32-bit addressing), starting at the ModRM byte.
int ModRmLen(const uint8_t* m) {
  uint8_t mod = m[0] >> 6, rm = m[0] & 7;
  int len = 1;
  if (mod == 3) return len;
  if (rm == 4) {
    uint8_t sib = m[1];
    ++len;
    if (mod == 0 && (sib & 7) == 5) len += 4;
  }
  if (mod == 0 && rm == 5) len += 4;
  else if (mod == 1) len += 1;
  else if (mod == 2) len += 4;
  return len;
}

uint32_t& Reg32(CONTEXT* c, int idx) {
  DWORD* regs[8] = {&c->Eax, &c->Ecx, &c->Edx, &c->Ebx, &c->Esp, &c->Ebp, &c->Esi, &c->Edi};
  return reinterpret_cast<uint32_t&>(*regs[idx & 7]);
}

const char* SegName(int s) {
  static const char* n[] = {"es", "cs", "ss", "ds", "fs", "gs", "?", "?"};
  return n[s & 7];
}

void HandleInt(uint8_t vec, CONTEXT* c) {
  Regs r{c};
  if (g_cfg.traceInts)
    Log("int %02x AX=%04x BX=%04x CX=%04x DX=%04x from %08x", vec, r.ax(), r.bx(), r.cx(), r.dx(),
        g_image->Orig(c->Eip));
  switch (vec) {
    case 0x21: Int21(r); break;
    case 0x31: Int31(r); break;
    case 0x33: Int33(r); break;
    case 0x16: Int16(r); break;
    case 0x10: Int10(r); break;
    case 0x2F: Int2F(r); break;
    default:
      Log("int %02x (AX=%04x) from %08x ignored", vec, r.ax(), g_image->Orig(c->Eip));
  }
}

// Returns true if the faulting instruction was emulated (EIP advanced).
bool Emulate(CONTEXT* c) {
  if (int skip = EmulatePatchedSite(c->Eip, reinterpret_cast<uint32_t*>(&c->Esp))) {
    c->Eip += skip;
    return true;
  }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(uintptr_t(c->Eip));
  Prefixes pre = ReadPrefixes(p);
  const uint8_t* op = p + pre.len;
  int portSize = pre.opsize ? 2 : 4;
  switch (op[0]) {
    case 0xCD:  // INT imm8
      c->Eip += pre.len + 2;   // advance first: handlers may exit / inspect return
      HandleInt(op[1], c);
      return true;
    case 0xFA: g_if = false; c->Eip += pre.len + 1; return true;  // CLI
    case 0xFB: g_if = true; c->Eip += pre.len + 1; return true;   // STI
    case 0xF4: Sleep(0); c->Eip += pre.len + 1; return true;      // HLT
    case 0xE4: c->Eax = (c->Eax & ~0xFFu) | (PortIn(op[1], 1) & 0xFF); c->Eip += pre.len + 2; return true;
    case 0xE5: {
      uint32_t v = PortIn(op[1], portSize);
      c->Eax = portSize == 2 ? (c->Eax & 0xFFFF0000u) | (v & 0xFFFF) : v;
      c->Eip += pre.len + 2;
      return true;
    }
    case 0xE6: PortOut(op[1], c->Eax & 0xFF, 1); c->Eip += pre.len + 2; return true;
    case 0xE7: PortOut(op[1], portSize == 2 ? c->Eax & 0xFFFF : c->Eax, portSize); c->Eip += pre.len + 2; return true;
    case 0xEC: c->Eax = (c->Eax & ~0xFFu) | (PortIn(uint16_t(c->Edx), 1) & 0xFF); c->Eip += pre.len + 1; return true;
    case 0xED: {
      uint32_t v = PortIn(uint16_t(c->Edx), portSize);
      c->Eax = portSize == 2 ? (c->Eax & 0xFFFF0000u) | (v & 0xFFFF) : v;
      c->Eip += pre.len + 1;
      return true;
    }
    case 0xEE: PortOut(uint16_t(c->Edx), c->Eax & 0xFF, 1); c->Eip += pre.len + 1; return true;
    case 0xEF: PortOut(uint16_t(c->Edx), portSize == 2 ? c->Eax & 0xFFFF : c->Eax, portSize); c->Eip += pre.len + 1; return true;
    case 0x6E: case 0x6F: {  // OUTSB / OUTSD (palette uploads use REP OUTSB)
      int sz = op[0] == 0x6E ? 1 : portSize;
      uint32_t n = pre.rep ? c->Ecx : 1;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, Mem(c->Esi), sz);
        PortOut(uint16_t(c->Edx), v, sz);
        c->Esi += sz;
      }
      if (pre.rep) c->Ecx = 0;
      c->Eip += pre.len + 1;
      return true;
    }
    case 0x6C: case 0x6D: {  // INSB / INSD
      int sz = op[0] == 0x6C ? 1 : portSize;
      uint32_t n = pre.rep ? c->Ecx : 1;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t v = PortIn(uint16_t(c->Edx), sz);
        std::memcpy(Mem(c->Edi), &v, sz);
        c->Edi += sz;
      }
      if (pre.rep) c->Ecx = 0;
      c->Eip += pre.len + 1;
      return true;
    }
    case 0x8E: {  // MOV Sreg, r/m16 with a DOS selector: use the flat selector
      int sreg = (op[1] >> 3) & 7;
      if (sreg == 4 || sreg == 5) {
        Log("note: load of %s ignored at %08x", SegName(sreg), g_image->Orig(c->Eip));
      } else {
        if (sreg == 0) c->SegEs = g_flatDs;
        else if (sreg == 2) c->SegSs = g_flatDs;
        else if (sreg == 3) c->SegDs = g_flatDs;
      }
      c->Eip += pre.len + 1 + ModRmLen(op + 1);
      return true;
    }
    case 0x07: c->SegEs = g_flatDs; c->Esp += 4; c->Eip += pre.len + 1; return true;  // POP ES
    case 0x1F: c->SegDs = g_flatDs; c->Esp += 4; c->Eip += pre.len + 1; return true;  // POP DS
    case 0x0F:
      if (op[1] == 0xA1 || op[1] == 0xA9) {  // POP FS/GS: keep Windows' FS
        c->Esp += 4;
        c->Eip += pre.len + 2;
        return true;
      }
      break;
    default:
      break;
  }
  return false;
}

// Delivers one pending timer tick by building the frame the CPU would push
// for a hardware interrupt (EFLAGS, CS, EIP) and entering the game's IRQ0
// handler. Only at instruction boundaries in game code with interrupts
// virtually enabled - exactly when a real PC could take the interrupt.
bool TryInjectIrq(CONTEXT* c) {
  if (g_irqPending <= 0 || !g_if) return false;
  const Vector& v = g_pmVectors[8];
  if (!v.off || !g_image->IsCodeHost(c->Eip)) return false;
  InterlockedDecrement(&g_irqPending);
  static unsigned delivered = 0;
  if (g_cfg.traceIrq && (++delivered % 200) == 1) {
    Log("irq0: %u ticks delivered (EIP orig %08x) counter=%u busy=%d lock=%d us/tick=%u", delivered,
        g_image->Orig(c->Eip), *g_image->Ptr<uint32_t>(0x122044), *g_image->Ptr<int32_t>(0x8893A),
        *g_image->Ptr<int32_t>(0x8893E), *g_image->Ptr<uint32_t>(0x88936));
    {
      char buf[512]; int n = 0;
      const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(c->Esp));
      for (int i = 0; i < 400 && n < 480; ++i)
        if (g_image->IsCodeHost(sp[i])) n += std::snprintf(buf + n, sizeof buf - n, " %05x", g_image->Orig(sp[i]));
      Log("   callers:%s", buf);
    }
    for (int i = 0; i < 2; ++i) {
      uint32_t st = g_image->Ptr<uint32_t>(0x887E8)[i];
      if (st) Log("   timer %2d status %u period %u acc %u pending %u cb %08x", i, st,
                  g_image->Ptr<uint32_t>(0x88868)[i], g_image->Ptr<uint32_t>(0x88828)[i],
                  g_image->Ptr<uint32_t>(0x888A8)[i], g_image->Orig(g_image->Ptr<uint32_t>(0x887A8)[i]));
    }
  }
  uint32_t* sp = reinterpret_cast<uint32_t*>(uintptr_t(c->Esp - 12));
  sp[0] = c->Eip;
  sp[1] = c->SegCs;
  sp[2] = c->EFlags & ~0x100u;
  c->Esp -= 12;
  c->Eip = v.off;
  c->EFlags &= ~0x100u;
  return true;
}

// Timer thread: generates ticks at the programmed PIT rate; if the game
// thread is running game code, sets its trap flag so the next instruction
// boundary raises a single-step exception, where the tick is delivered.
DWORD WINAPI TickerThread(void*) {
  timeBeginPeriod(1);
  LARGE_INTEGER freq, last, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&last);
  double acc = 0;
  for (;;) {
    Sleep(1);
    QueryPerformanceCounter(&now);
    double period = double(g_pitDivisor) / 1193182.0;
    acc += double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
    last = now;
    while (acc >= period) {
      acc -= period;
      if (g_pmVectors[8].off && g_irqPending < 8) InterlockedIncrement(&g_irqPending);
    }
    if (g_irqPending > 0 && g_if && g_pmVectors[8].off) {
      if (SuspendThread(g_gameThread) == DWORD(-1)) continue;
      CONTEXT ctx;
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(g_gameThread, &ctx) && g_image->IsCodeHost(ctx.Eip) && !(ctx.EFlags & 0x100)) {
        ctx.EFlags |= 0x100;
        SetThreadContext(g_gameThread, &ctx);
      }
      ResumeThread(g_gameThread);
    }
  }
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
  CONTEXT* c = ep->ContextRecord;
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (g_watchAddr && !g_watchArmed) {
    c->Dr0 = g_watchAddr;
    c->Dr7 = 0x00010001;  // L0, break on 1-byte write
    g_watchArmed = true;
    Log("watch armed at %08x", g_watchAddr);
  }
  if (code == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1)) {
    char buf[512]; int n = 0;
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(uintptr_t(c->Esp));
    for (int i = 0; i < 300 && n < 480; ++i)
      if (g_image->IsCodeHost(sp[i])) n += std::snprintf(buf + n, sizeof buf - n, " %05x", g_image->Orig(sp[i]));
    static int hits = 0;
    if (hits++ < 12) Log("WATCH hit: EIP orig %08x val=%02x callers:%s", g_image->Orig(c->Eip),
                         *reinterpret_cast<uint8_t*>(uintptr_t(g_watchAddr)), buf);
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && g_stepBudget == 0) {
    // trap flag set by the timer thread: deliver the interrupt now
    c->EFlags &= ~0x100u;
    TryInjectIrq(c);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && g_stepBudget > 0) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(uintptr_t(c->Eip));
    Log("step %08x (orig %08x) %02x %02x %02x %02x  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esp=%08lx",
        c->Eip, g_image->Orig(c->Eip), p[0], p[1], p[2], p[3], c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esp);
    if (--g_stepBudget > 0) c->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (!g_image || !g_image->IsCodeHost(c->Eip)) return EXCEPTION_CONTINUE_SEARCH;
  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_PRIV_INSTRUCTION ||
      code == EXCEPTION_ILLEGAL_INSTRUCTION) {
    if (Emulate(c)) {
      TryInjectIrq(c);
      if (g_cfg.stepAfterTrap > 0) { g_stepBudget = g_cfg.stepAfterTrap; c->EFlags |= 0x100; }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->ExceptionInformation[1] == 0xFFFFFFFFu &&
        ((c->SegDs & 0xFFFF) != g_flatDs || (c->SegEs & 0xFFFF) != g_flatDs)) {
      // General protection fault through a DOS-style selector left in DS/ES:
      // everything is flat on Windows, so repair and retry the instruction.
      Log("note: repaired DS=%04lx ES=%04lx at %08x", c->SegDs, c->SegEs, g_image->Orig(c->Eip));
      c->SegDs = g_flatDs;
      c->SegEs = g_flatDs;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
      char what[128];
      std::snprintf(what, sizeof what, "memory access violation (%s address %08lx)",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    ep->ExceptionRecord->ExceptionInformation[1]);
      Fatal(c, what);
    }
    Fatal(c, "unsupported privileged instruction");
  }
  if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) Fatal(c, "division by zero");
  if (code == EXCEPTION_STACK_OVERFLOW) Fatal(c, "stack overflow");
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void DosSetWatch(uint32_t hostAddr) { g_watchAddr = hostAddr; g_watchArmed = false; }

void DosInstall(GameImage* image, const DosConfig& cfg) {
  g_image = image;
  g_cfg = cfg;
  uint16_t ds, cs;
  __asm__ volatile("mov %%ds, %0" : "=r"(ds));
  __asm__ volatile("mov %%cs, %0" : "=r"(cs));
  g_flatDs = ds;
  g_codeSel = cs;
  g_iretStub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  g_iretStub[0] = 0xCF;  // iretd
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gameThread,
                  THREAD_ALL_ACCESS, FALSE, 0);

  g_files.assign(5, DosFile{});
  g_files[0] = {GetStdHandle(STD_INPUT_HANDLE), true};
  g_files[1] = {GetStdHandle(STD_OUTPUT_HANDLE), true};
  g_files[2] = {GetStdHandle(STD_ERROR_HANDLE), true};
  g_files[3] = {GetStdHandle(STD_ERROR_HANDLE), true};  // stdaux
  g_files[4] = {GetStdHandle(STD_ERROR_HANDLE), true};  // stdprn

  // Low-memory areas the game may address directly, as under DOS/4GW:
  // DOS memory blocks (DPMI 0100h) and the VGA/VESA window at 0xA0000.
  g_dosArenaOk = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(kDosArenaBase)), kDosArenaSize,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr;
  bool vga = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(0xA0000)), 0x20000,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr;
  Log("low memory: DOS arena %s, VGA window %s", g_dosArenaOk ? "ok" : "UNAVAILABLE",
      vga ? "ok" : "UNAVAILABLE");

  video::SetInputListener(SyncMouseToBios);
  AddVectoredExceptionHandler(1, VectoredHandler);
  CreateThread(nullptr, 0, TickerThread, nullptr, 0, nullptr);
}

}  // namespace asc
