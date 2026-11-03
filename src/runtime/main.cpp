// Ascendancy native runner (Phase 1).
//
// Loads the original ASCEND.EXE, sets up the state the Watcom C runtime
// expects, and runs the game code natively. DOS/hardware access is emulated
// by runtime/dos.cpp.
//
// Usage: Ascendancy.exe [--game <dir>] [--trace]
//   The game directory defaults to the folder containing Ascendancy.exe.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "assets/cob.h"
#include "runtime/dos.h"
#include "runtime/settings.h"
#include "runtime/game_image.h"
#include "runtime/patches.h"
#include "runtime/vfx_driver.h"
#include "runtime/video.h"

namespace asc::video { extern uint32_t (*g_debugPeek)(uint32_t); }

namespace {

using asc::GameImage;
using asc::Log;
namespace addr = asc::addr;

// Calls a Watcom register-convention function: args in EAX, EDX, EBX, ECX.
uint32_t CallWatcom(uint32_t fn, uint32_t a = 0, uint32_t d = 0, uint32_t b = 0, uint32_t c = 0) {
  uint32_t result;
  __asm__ volatile(
      "push %%ebp\n\t"
      "call *%%esi\n\t"
      "pop %%ebp\n\t"
      : "=a"(result), "+d"(d), "+b"(b), "+c"(c), "+S"(fn)
      : "a"(a)
      : "edi", "memory", "cc");
  return result;
}

std::string ExeDir() {
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s(path, n);
  return s.substr(0, s.find_last_of("\\/"));
}

bool FileExists(const std::string& p) {
  DWORD a = GetFileAttributesA(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Strings that must outlive the game: command line, program name, environment.
char g_cmdLine[256];
char g_pgmName[MAX_PATH];
char g_env[512];

void SetupRuntimeGlobals(GameImage& img, const std::string& gameDir) {
  uint16_t ds;
  __asm__ volatile("mov %%ds, %0" : "=r"(ds));

  // Stack: the game runs on this thread's stack (reserved large by the linker).
  NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  uint32_t stackLow = uint32_t(uintptr_t(tib->StackLimit));
  uint32_t stackBase = uint32_t(uintptr_t(tib->StackBase));
  // The committed limit grows downwards as the stack is touched; tell the
  // Watcom stack checks about the whole reservation instead.
  MEMORY_BASIC_INFORMATION mbi{};
  VirtualQuery(tib->StackLimit, &mbi, sizeof mbi);
  stackLow = uint32_t(uintptr_t(mbi.AllocationBase)) + 64 * 1024;  // keep a guard margin

  std::snprintf(g_pgmName, sizeof g_pgmName, "%s\\ASCEND.EXE", gameDir.c_str());
  g_cmdLine[0] = 0;
  // Environment block: NAME=value\0 ... \0
  const char env[] = "PATH=C:\\\0COMSPEC=C:\\COMMAND.COM\0DOS4G=QUIET\0";
  std::memcpy(g_env, env, sizeof env);

  *img.Ptr<uint32_t>(addr::kStackTop) = stackBase;
  *img.Ptr<uint32_t>(addr::kInitialEsp2) = stackBase;
  *img.Ptr<uint32_t>(addr::kInitialEsp) = stackBase;
  *img.Ptr<uint32_t>(addr::kStackLow) = stackLow;
  *img.Ptr<uint32_t>(addr::kLpCmdLine) = uint32_t(uintptr_t(g_cmdLine));
  *img.Ptr<uint32_t>(addr::kLpPgmName) = uint32_t(uintptr_t(g_pgmName));
  *img.Ptr<uint16_t>(addr::kPspSel) = ds;
  *img.Ptr<uint8_t>(addr::kNo87) = 0;
  *img.Ptr<uint8_t>(addr::kExtender) = 1;  // DOS/4GW
  *img.Ptr<uint8_t>(addr::kExtSubtype) = 0;
  *img.Ptr<uint32_t>(addr::kEnvPtr) = uint32_t(uintptr_t(g_env));
  *img.Ptr<uint16_t>(addr::kEnvSel) = ds;
  *img.Ptr<uint8_t>(addr::kDosMajor) = 6;
  *img.Ptr<uint8_t>(addr::kDosMinor) = 22;
  // __interrupt handlers reload DS from this slot inside the code object.
  *img.Ptr<uint16_t>(addr::kGetDsSlot) = ds;
}

}  // namespace

int main(int argc, char** argv) {
  std::string gameDir = ExeDir();
  asc::DosConfig cfg;
  asc::video::Config vcfg;
  std::string script;
  bool traceVfx = false;
  bool forceWindowed = false, forceFullscreen = false, noRightHelp = false;
  std::string forceFilter;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--game") && i + 1 < argc) gameDir = argv[++i];
    else if (!std::strcmp(argv[i], "--trace")) cfg.traceInts = true;
    else if (!std::strcmp(argv[i], "--step") && i + 1 < argc) cfg.stepAfterTrap = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--trace-irq")) cfg.traceIrq = true;
    else if (!std::strcmp(argv[i], "--trace-vfx")) traceVfx = true;
    else if (!std::strcmp(argv[i], "--windowed")) forceWindowed = true;
    else if (!std::strcmp(argv[i], "--fullscreen")) forceFullscreen = true;
    else if (!std::strcmp(argv[i], "--smooth")) forceFilter = "soft";
    else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) forceFilter = argv[++i];
    else if (!std::strcmp(argv[i], "--no-right-help")) noRightHelp = true;
    else if (!std::strcmp(argv[i], "--headless")) vcfg.headless = true;
    else if (!std::strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
    else if (!std::strcmp(argv[i], "--shots") && i + 1 < argc) vcfg.screenshotEveryMs = std::atoi(argv[++i]);
  }
  while (!gameDir.empty() && (gameDir.back() == '\\' || gameDir.back() == '/')) gameDir.pop_back();
  cfg.gameDir = gameDir;

  std::string exe = gameDir + "\\ASCEND.EXE";
  if (!FileExists(exe)) {
    std::string msg = "ASCEND.EXE was not found in\n" + gameDir +
                      "\n\nPut Ascendancy.exe in your Ascendancy folder, or start it with "
                      "--game <folder>.";
    MessageBoxA(nullptr, msg.c_str(), "Ascendancy", MB_ICONERROR);
    return 1;
  }

  static GameImage image;
  std::string err;
  if (!image.Load(exe, &err)) {
    MessageBoxA(nullptr, err.c_str(), "Ascendancy", MB_ICONERROR);
    return 1;
  }
  Log("loaded %s (%s placement)", exe.c_str(), image.atPreferredBase() ? "standard" : "relocated");
  for (size_t i = 0; i < image.le().objects().size(); ++i) {
    uint32_t b = image.le().objects()[i].preferredBase;
    Log("  object %zu: orig %08x -> host %08x", i + 1, b, image.Host(b));
  }

  vcfg.screenshotDir = gameDir;
  // Settings: ascendancy.ini (written by the in-game options panel), with
  // command-line options taking precedence for this session.
  vcfg.settingsPath = gameDir + "\\ascendancy.ini";
  vcfg.settings.Load(vcfg.settingsPath);
  if (forceWindowed) vcfg.settings.fullscreen = false;
  if (forceFullscreen) vcfg.settings.fullscreen = true;
  if (noRightHelp) vcfg.settings.rightClickHelp = false;
  for (int f = 0; f < int(asc::Filter::kCount); ++f)
    if (forceFilter == asc::FilterKey(asc::Filter(f))) vcfg.settings.filter = asc::Filter(f);
  {
    asc::CobFileSystem fs;
    std::string ferr;
    if (fs.MountFromConfig(gameDir, &ferr)) {
      fs.ReadFile("data\\bigfont.fnt", &vcfg.bigFont);
      fs.ReadFile("data\\smfont.fnt", &vcfg.smallFont);
    }
  }
  if (!asc::ApplyPatches(image) || !asc::InstallVfxDriver(image, traceVfx)) {
    MessageBoxA(nullptr, "This version of ASCEND.EXE is not supported yet (see ascendancy.log).",
                "Ascendancy", MB_ICONERROR);
    return 1;
  }
  asc::DosInstall(&image, cfg);
  if (!asc::video::Init(vcfg)) {
    MessageBoxA(nullptr, "Could not open the game window (see ascendancy.log).", "Ascendancy",
                MB_ICONERROR);
    return 1;
  }
  if (!script.empty()) asc::video::SetInputScript(script);
  // Current game screen: u16 in the window manager object (0 = main menu).
  asc::video::SetScreenQuery([] { return int(*image.Ptr<uint16_t>(0xFE584)); });
  asc::video::g_debugPeek = [](uint32_t a) { return *image.Ptr<uint32_t>(a); };
  SetupRuntimeGlobals(image, gameDir);
  SetCurrentDirectoryA(gameDir.c_str());

  Log("running C runtime initialisers");
  CallWatcom(image.Host(addr::kInitRtns), 0xFF);
  Log("entering game main()");
  CallWatcom(image.Host(addr::kCMain));  // does not return: calls exit()
  return 0;
}
