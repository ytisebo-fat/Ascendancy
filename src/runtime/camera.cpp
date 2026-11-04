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
uint32_t g_batWnd = 0;            // last BatModeWnd (system view / battles)
uint32_t g_batHandler = 0;
uint32_t g_frameFunction = 0;     // host address of the original frame function
void __cdecl CameraBeforeFrame();
void CosmosHandlerThunk();
void BatHandlerThunk();
void FrameThunk();
}

namespace asc {
namespace {

constexpr uint32_t kCosmosHandlerSlot = 0x85A54;  // vtable entry -> 0x21F84
constexpr uint32_t kCosmosHandler = 0x21F84;
constexpr uint32_t kFrameCallSite = 0x1CC03;      // call 0x4FAA0 (per-frame, main loop)
constexpr uint32_t kModalFrameCallSite = 0x50A49; // same call in the modal loop (winmgr 0x509DC)
constexpr uint32_t kFrameFunction = 0x4FAA0;
constexpr uint32_t kRotate = 0x23834, kZoom = 0x23878, kUpdateView = 0x2390C;
constexpr uint32_t kRecenter = 0x21EEC;  // (window EAX): zoom 35, default orientation

// System view / battles (BatModeWnd, screen 16; batmode.cpp handler 0x16348,
// vtable entry 0x858F0). Messages 50/51 rotate, 54/55 zoom, 71 recentre; each
// frame message 59 calls:
//   0x17E50 rotate(window EAX, int degrees EDX)    (+-2 per frame)
//   0x182B8 zoom(window EAX, EDX)                 (+-10 per frame)
//   0x18C2C redraw(window EAX, 0)
//   0x17F54 recentre(window EAX)
constexpr uint32_t kBatHandlerSlot = 0x858F0, kBatHandler = 0x16348;
constexpr uint32_t kBatRotate = 0x17E50, kBatZoom = 0x182B8, kBatRedraw = 0x18C2C, kBatRecenter = 0x17F54;
constexpr int kScreenSystem = 16;
constexpr int kBatZoomStep = 10;
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
    "_BatHandlerThunk:\n"
    "  movl %eax, _g_batWnd\n"
    "  jmp *_g_batHandler\n"
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

// List windows (windows.txt TYPE 10: the diplomacy phrases, save games, ship
// components, status lists, ...) are one class, gwindow.cpp, vtable 0x85C70.
// Its message handler (vtable slot 2, 0x2DAB4) scrolls one line per message
// 0x191 (up) / 0x192 (down) - the auto-repeat messages of its arrow buttons -
// and redraws itself. Fields: rectangle at +8 (x0,y0,x1,y1), item count u16
// at +0x8C7, line height u16 at +0x8C9, top line u16 at +0x8CD.
// The window manager (0xFA35C) keeps a table of all windows at +0 (12-byte
// entries: reference count, window, 0; up to 0x400) and, per screen
// (+0x3064 + screen * 0x8E), up to 32 table indices at +0x306A. The current
// screen is at +0x4228. The game finds e.g. PLAYERDIP the same way (0x521DC).
constexpr uint32_t kListVtable = 0x85C70;
constexpr uint32_t kWinMgr = 0xFA35C;
constexpr uint32_t kScreenOffset = 0x4228, kScreenSlots = 0x306A, kScreenStride = 0x8E;
constexpr uint32_t kMsgScrollUp = 0x191, kMsgScrollDown = 0x192;

template <typename T>
T Field(uint32_t obj, uint32_t off) {
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(uintptr_t(obj + off)), sizeof v);
  return v;
}

void SendMessage3(uint32_t wnd, uint32_t msg) {
  uint32_t handler = *reinterpret_cast<uint32_t*>(uintptr_t(Field<uint32_t>(wnd, 0xA7) + 8));
  uint32_t ebx = 0;
  __asm__ volatile("call *%[fn]\n\t"
                   : "+a"(wnd), "+d"(msg), "+b"(ebx)
                   : [fn] "S"(handler)
                   : "ecx", "memory", "cc");
}

// Scrolls the shown list window under (x, y) by `notches` (+ = down).
void ScrollListAt(int x, int y, int notches) {
  uint32_t vtable = g_image->Host(kListVtable);
  int screen = *g_image->Ptr<int16_t>(kWinMgr + kScreenOffset);
  if (screen < 0 || screen > 31) return;
  for (int k = 0; k < 32; ++k) {
    int idx = *g_image->Ptr<int32_t>(kWinMgr + kScreenSlots + uint32_t(screen) * kScreenStride + k * 4);
    if (idx <= 0 || idx >= 0x400) continue;
    uint32_t obj = *g_image->Ptr<uint32_t>(kWinMgr + 4 + uint32_t(idx) * 12);
    if (obj < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(uintptr_t(obj)), 0x8D0)) continue;
    if (Field<uint32_t>(obj, 0) != 0x12345678 || Field<uint32_t>(obj, 0xA7) != vtable) continue;
    int x0 = Field<int32_t>(obj, 8), y0 = Field<int32_t>(obj, 0xC);
    int x1 = Field<int32_t>(obj, 0x10), y1 = Field<int32_t>(obj, 0x14);
    if (x < x0 || x > x1 || y < y0 || y > y1) continue;
    int lineH = Field<uint16_t>(obj, 0x8C9);
    int visible = lineH > 0 ? (y1 - y0) / lineH : 1;
    int perNotch = visible >= 10 ? 3 : 1;  // long lists move faster
    int n = notches * perNotch;
    uint32_t msg = n > 0 ? kMsgScrollDown : kMsgScrollUp;
    static int logged = 0;
    if (logged < 3 && ++logged)
      Log("wheel: scrolling list %.10s (table index %d, screen %d)",
          reinterpret_cast<const char*>(uintptr_t(obj + 0x20)), idx, screen);
    for (int i = n > 0 ? n : -n; i > 0; --i) SendMessage3(obj, msg);
    return;
  }
  static int missed = 0;
  if (missed < 5 && ++missed) Log("wheel: no list under %d,%d on screen %d", x, y, screen);
}

}  // namespace
}  // namespace asc

using namespace asc;

namespace {
float g_batYaw = 0;  // fractional degrees not yet applied (system view rotates in whole degrees)

void SystemViewFrame(const video::CameraInput& in) {
  using namespace asc;
  uint32_t w = g_batWnd;
  g_zoomPending += in.zoomSteps;
  if (g_zoomPending > 12) g_zoomPending = 12;
  if (g_zoomPending < -12) g_zoomPending = -12;
  bool changed = false;
  if (in.recenter) {
    CallWindowFn(g_image->Host(kBatRecenter), w);
    g_zoomPending = 0;
    g_batYaw = 0;
    changed = true;
  }
  g_batYaw += in.yawDegrees;
  int whole = int(g_batYaw);
  if (whole != 0) {
    CallWindowFn(g_image->Host(kBatRotate), w, uint32_t(whole));
    g_batYaw -= float(whole);
    changed = true;
  }
  if (g_zoomPending != 0) {
    int dir = g_zoomPending > 0 ? 1 : -1;
    CallWindowFn(g_image->Host(kBatZoom), w, uint32_t(dir * kBatZoomStep));
    g_zoomPending -= dir;
    changed = true;
  }
  if (changed) CallWindowFn(g_image->Host(kBatRedraw), w, 0);
}
}  // namespace

extern "C" void __cdecl CameraBeforeFrame() {
  video::CameraInput in = video::TakeCameraInput();
  if (in.listScroll != 0) ScrollListAt(in.listX, in.listY, in.listScroll);
  int screen = *g_image->Ptr<uint16_t>(0xFE584);
  if (screen == kScreenSystem && g_batWnd) {
    SystemViewFrame(in);
    return;
  }
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
  for (uint32_t at : {kFrameCallSite, kModalFrameCallSite}) {
    const uint8_t* callSite = &code.data[at - code.preferredBase];
    int32_t rel;
    std::memcpy(&rel, callSite + 1, 4);
    if (callSite[0] != 0xE8 || at + 5 + uint32_t(rel) != kFrameFunction) {
      Log("camera: frame call site not as expected - camera controls disabled");
      return false;
    }
  }
  uint32_t* slot = image.Ptr<uint32_t>(kCosmosHandlerSlot);
  uint32_t* batSlot = image.Ptr<uint32_t>(kBatHandlerSlot);
  if (*slot != image.Host(kCosmosHandler) || *batSlot != image.Host(kBatHandler)) {
    Log("camera: window vtables not as expected - camera controls disabled");
    return false;
  }
  g_cosmosHandler = image.Host(kCosmosHandler);
  g_batHandler = image.Host(kBatHandler);
  g_frameFunction = image.Host(kFrameFunction);
  *slot = uint32_t(uintptr_t(&CosmosHandlerThunk));
  *batSlot = uint32_t(uintptr_t(&BatHandlerThunk));
  for (uint32_t at : {kFrameCallSite, kModalFrameCallSite}) {
    uint8_t* site = image.Ptr<uint8_t>(at);
    int32_t newRel = int32_t(uintptr_t(&FrameThunk) - (uintptr_t(site) + 5));
    std::memcpy(site + 1, &newRel, 4);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
  }
  video::AddCameraArea(7, 7, 465, 472, kScreenStarMap);  // COSMOSWnd
  video::AddCameraArea(7, 7, 465, 472, kScreenSystem);   // BatModeWnd
  return true;
}

}  // namespace asc
