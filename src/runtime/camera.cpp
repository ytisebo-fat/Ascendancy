// Camera controls for the star map's 3D galaxy view.
//
// The game's camera buttons (windows.txt: ROTLEFT/ROTRIGHT/ROTUP/ROTDOWN/
// ZOOMIN/ZOOMOUT, messages 50-55 to COSMOSWnd) only toggle continuous motion;
// each frame the window's message handler (coswnd 0x21F84, message 59) then
// calls:
//   0x23834  rotate(window EAX, axis EDX: -1 spin / 0 tilt, float degrees) ret 4
//   0x23878  zoom(window EAX, EDX: -1 closer / +1 further)   distance 20..60
//   0x2390C  recompute the view(window EAX)
//   [window+0xA7]->vtable[3](window, 0)  redraw
// The port calls the same functions with wheel/drag input, once per game frame
// from a hook in the main loop (the call to the frame function at 0x1CC03), so
// they always run on the game thread at a well-defined point.
// The COSMOSWnd instance is captured through its vtable entry for the message
// handler (data 0x85A54).
#include "runtime/camera.h"

#include <windows.h>

#include <cstring>

#include "runtime/dos.h"
#include "runtime/game_image.h"
#include "runtime/video.h"

// Shared with the assembly thunks below (C linkage, global scope).
extern "C" {
uint32_t g_cosmosWnd = 0;         // last COSMOSWnd seen by its message handler
uint32_t g_cosmosHandler = 0;     // host address of the original handler
uint32_t g_frameFunction = 0;     // host address of the original frame function
void __cdecl CameraBeforeFrame();
void CosmosHandlerThunk();
void FrameThunk();
}

namespace asc {
namespace {

constexpr uint32_t kCosmosHandlerSlot = 0x85A54;  // vtable entry -> 0x21F84
constexpr uint32_t kCosmosHandler = 0x21F84;
constexpr uint32_t kFrameCallSite = 0x1CC03;      // call 0x4FAA0 (per-frame)
constexpr uint32_t kFrameFunction = 0x4FAA0;
constexpr uint32_t kRotate = 0x23834, kZoom = 0x23878, kUpdateView = 0x2390C;
constexpr uint32_t kRecenter = 0x21EEC;  // (window EAX): zoom 35, default orientation
constexpr int kScreenStarMap = 1;

GameImage* g_image = nullptr;

// Message-handler thunk: remember the window (EAX), continue in the original.
// Frame thunk: run CameraBeforeFrame with all registers preserved, then
// continue in the original frame function (EAX = window manager).
__asm__(
    ".text\n"
    "_CosmosHandlerThunk:\n"
    "  movl %eax, _g_cosmosWnd\n"
    "  jmp *_g_cosmosHandler\n"
    "_FrameThunk:\n"
    "  pushal\n"
    "  pushfl\n"
    "  cld\n"
    "  call _CameraBeforeFrame\n"
    "  popfl\n"
    "  popal\n"
    "  jmp *_g_frameFunction\n");

// Calls into Watcom-convention game code. The callee preserves EBX, ESI, EDI
// and EBP; EAX/EDX carry arguments and may change; ECX is treated as clobbered.
// Function address and stack argument are passed in registers: a memory
// operand could be ESP-relative and would be wrong after the push.
void Rotate(uint32_t wnd, int axis, float degrees) {
  uint32_t fn = g_image->Host(kRotate);
  uint32_t bits;
  std::memcpy(&bits, &degrees, 4);
  __asm__ volatile(
      "pushl %[deg]\n\t"
      "call *%[fn]\n\t"
      : "+a"(wnd), "+d"(axis)
      : [deg] "D"(bits), [fn] "S"(fn)
      : "ecx", "memory", "cc");
}

void Zoom(uint32_t wnd, int dir) {
  uint32_t fn = g_image->Host(kZoom);
  __asm__ volatile("call *%[fn]\n\t"
                   : "+a"(wnd), "+d"(dir)
                   : [fn] "S"(fn)
                   : "ecx", "memory", "cc");
}

void CallWindowFn(uint32_t fn, uint32_t wnd, uint32_t edx = 0) {
  __asm__ volatile("call *%[fn]\n\t"
                   : "+a"(wnd), "+d"(edx)
                   : [fn] "S"(fn)
                   : "ecx", "memory", "cc");
}

void UpdateAndRedraw(uint32_t wnd) {
  CallWindowFn(g_image->Host(kUpdateView), wnd);
  uint32_t vtable;
  std::memcpy(&vtable, reinterpret_cast<void*>(uintptr_t(wnd + 0xA7)), 4);
  uint32_t redraw = *reinterpret_cast<uint32_t*>(uintptr_t(vtable + 0xC));
  CallWindowFn(redraw, wnd, 0);
}

void Recenter(uint32_t wnd) { CallWindowFn(g_image->Host(kRecenter), wnd); }

int g_zoomPending = 0;  // in zoom steps (1 unit each), applied one per frame

}  // namespace
}  // namespace asc

using namespace asc;

extern "C" void __cdecl CameraBeforeFrame() {
  video::CameraInput in = video::TakeCameraInput();
  int screen = *g_image->Ptr<uint16_t>(0xFE584);
  if (screen != kScreenStarMap || !g_cosmosWnd) {
    g_zoomPending = 0;
    return;
  }
  g_zoomPending += in.zoomSteps;
  if (g_zoomPending > 12) g_zoomPending = 12;
  if (g_zoomPending < -12) g_zoomPending = -12;
  bool changed = false;
  if (in.recenter) { Recenter(g_cosmosWnd); g_zoomPending = 0; changed = true; }
  if (in.yawDegrees != 0.0f) { Rotate(g_cosmosWnd, -1, in.yawDegrees); changed = true; }
  if (in.tiltDegrees != 0.0f) { Rotate(g_cosmosWnd, 0, in.tiltDegrees); changed = true; }
  if (g_zoomPending != 0) {
    int dir = g_zoomPending > 0 ? 1 : -1;
    Zoom(g_cosmosWnd, dir);
    g_zoomPending -= dir;
    changed = true;
  }
  if (changed) UpdateAndRedraw(g_cosmosWnd);
}

namespace asc {

bool InstallCameraControls(GameImage& image) {
  g_image = &image;
  // Verify the original bytes: vtable entry and the call instruction.
  const auto& objs = image.le().objects();
  const LeObject& code = objs[0];
  const uint8_t* callSite = &code.data[kFrameCallSite - code.preferredBase];
  int32_t rel;
  std::memcpy(&rel, callSite + 1, 4);
  if (callSite[0] != 0xE8 || kFrameCallSite + 5 + uint32_t(rel) != kFrameFunction) {
    Log("camera: main-loop call site not as expected - camera controls disabled");
    return false;
  }
  uint32_t* slot = image.Ptr<uint32_t>(kCosmosHandlerSlot);
  if (*slot != image.Host(kCosmosHandler)) {
    Log("camera: COSMOSWnd vtable not as expected - camera controls disabled");
    return false;
  }
  g_cosmosHandler = image.Host(kCosmosHandler);
  g_frameFunction = image.Host(kFrameFunction);
  *slot = uint32_t(uintptr_t(&CosmosHandlerThunk));
  uint8_t* site = image.Ptr<uint8_t>(kFrameCallSite);
  int32_t newRel = int32_t(uintptr_t(&FrameThunk) - (uintptr_t(site) + 5));
  std::memcpy(site + 1, &newRel, 4);
  FlushInstructionCache(GetCurrentProcess(), site, 5);
  video::SetCameraArea(7, 7, 465, 472, kScreenStarMap);
  return true;
}

}  // namespace asc
