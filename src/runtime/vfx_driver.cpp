// Replacement for VESA480.DLL, the Miles Design VFX 3 video driver the game
// loads at run time. Instead of programming a VESA card with 64 KB banks, the
// 13 driver entry points draw into a linear 640x480 framebuffer that
// runtime/video.cpp shows in a window.
//
// Driver table order in this VFX version (verified by disassembling
// VESA480.DLL and the game's call sites; all cdecl):
//   0 describe  1 init  2 shutdown  3 area_wipe  4 wait_vblank_leading
//   5 wait_vblank  6 window_refresh  7 window_read  8 DAC_read  9 DAC_write
//   10 bank_reset  11 pane_refresh  12 line_address
#include "runtime/vfx_driver.h"

#include <cstring>
#include <vector>

#include "runtime/dos.h"
#include "runtime/game_image.h"
#include "runtime/hooks.h"
#include "runtime/video.h"

namespace asc {
namespace video { extern uint32_t g_debugOffscreenWindowHost; }
namespace {

using video::kHeight;
using video::kWidth;

struct VfxWindow {
  uint8_t* buffer;
  int32_t xMax, yMax;   // width-1, height-1 (stride = xMax+1)
  uint8_t* stencil;
  uint8_t* shadow;
};
struct VfxPane {
  VfxWindow* window;
  int32_t x0, y0, x1, y1;
};

// VFX_DESC: screen width, height, bytes/pixel, colours, pages, flags.
int32_t g_desc[8] = {kWidth, kHeight, 1, 256, 1, 1, 0, 0};
GameImage* g_image = nullptr;
bool g_trace = false;
#define VFX_TRACE(...) do { static int n_ = 0; if (g_trace && n_++ < 400) Log(__VA_ARGS__); } while (0)

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------------------
// Mouse pointer. The game's pointer module (0x5FCAC draw, 0x5FF2E erase)
// saves the background into the window at 0x1220B8, composites the pointer
// shape over it in the window at 0x1220D0, and refreshes that to the screen.
// The port keeps the pointer out of the framebuffer altogether: the render
// thread draws it as an overlay at the live mouse position. The shape is
// rendered with the game's own VFX shape routine.
// ---------------------------------------------------------------------------
constexpr uint32_t kCursorBgWindow = 0x1220B8;
constexpr uint32_t kCursorCompWindow = 0x1220D0;
constexpr uint32_t kCursorShapes = 0x12208C, kCursorFrame = 0x122074;
constexpr uint32_t kCursorW = 0x122090, kCursorH = 0x1220CC;
constexpr uint32_t kCursorHotX = 0x1220B0, kCursorHotY = 0x1220B4;
constexpr uint32_t kCursorShowCount = 0x1220AC, kCursorHidden = 0x1220E4;
constexpr uint32_t kVfxShapeDraw = 0x57EFC;  // (PANE*, shapes, frame, x, y) cdecl

template <typename T> T& Var(uint32_t a) { return *g_image->Ptr<T>(a); }

void CaptureCursor() {
  static uint32_t lastShapes = 0;
  static int32_t lastFrame = -1, lastW = 0, lastH = 0, lastHx = 0, lastHy = 0;
  uint32_t shapes = Var<uint32_t>(kCursorShapes);
  int32_t frame = Var<int32_t>(kCursorFrame);
  int32_t w = Var<int32_t>(kCursorW), h = Var<int32_t>(kCursorH);
  int32_t hx = Var<int32_t>(kCursorHotX), hy = Var<int32_t>(kCursorHotY);
  if (shapes == lastShapes && frame == lastFrame && w == lastW && h == lastH && hx == lastHx && hy == lastHy)
    return;
  if (w <= 0 || h <= 0 || w > 256 || h > 256 || !shapes) return;
  lastShapes = shapes; lastFrame = frame; lastW = w; lastH = h; lastHx = hx; lastHy = hy;

  using ShapeDraw = void(__cdecl*)(void*, void*, int32_t, int32_t, int32_t);
  ShapeDraw draw = reinterpret_cast<ShapeDraw>(uintptr_t(g_image->Host(kVfxShapeDraw)));
  std::vector<uint8_t> a(size_t(w) * h, 0x00), b(size_t(w) * h, 0xFF), mask(size_t(w) * h);
  VfxWindow wa{a.data(), w - 1, h - 1, nullptr, nullptr}, wb{b.data(), w - 1, h - 1, nullptr, nullptr};
  VfxPane pa{&wa, 0, 0, w - 1, h - 1}, pb{&wb, 0, 0, w - 1, h - 1};
  draw(&pa, reinterpret_cast<void*>(uintptr_t(shapes)), frame, -hx, -hy);
  draw(&pb, reinterpret_cast<void*>(uintptr_t(shapes)), frame, -hx, -hy);
  // Transparent where both differently-filled copies still show their fill.
  for (size_t i = 0; i < a.size(); ++i) mask[i] = !(a[i] == 0x00 && b[i] == 0xFF);
  video::SetCursorImage(a, mask, w, h, hx, hy, frame);
  Log("pointer: shape %d (%dx%d, hotspot %d,%d)", frame, w, h, -hx, -hy);
}

bool CursorVisible() {
  return Var<int32_t>(kCursorShowCount) >= 0 && Var<int32_t>(kCursorHidden) == 0;
}

// Copies a source rectangle of a window to a screen rectangle, scaling with
// nearest-neighbour when sizes differ (as the original driver does). As in
// the original, the destination is clipped first and the source is mapped
// onto the clipped area.
void Blit(const VfxWindow* w, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1,
          int dy1) {
  if (dx0 > kWidth - 1 || dy0 > kHeight - 1 || dx1 < 0 || dy1 < 0) return;
  dx0 = Clamp(dx0, 0, kWidth - 1); dx1 = Clamp(dx1, 0, kWidth - 1);
  dy0 = Clamp(dy0, 0, kHeight - 1); dy1 = Clamp(dy1, 0, kHeight - 1);
  if (dx1 < dx0 || dy1 < dy0 || sx1 < sx0 || sy1 < sy0) return;
  const int stride = w->xMax + 1;
  uint8_t* fb = video::Framebuffer();
  const int dw = dx1 - dx0 + 1, dh = dy1 - dy0 + 1;
  const int sw = sx1 - sx0 + 1, sh = sy1 - sy0 + 1;
  if (dw == sw && dh == sh) {
    for (int y = 0; y < dh; ++y) {
      const uint8_t* src = w->buffer + (sy0 + y) * stride + sx0;
      std::memcpy(fb + (dy0 + y) * kWidth + dx0, src, dw);
      if (w->shadow) std::memcpy(w->shadow + (sy0 + y) * stride + sx0, src, dw);
    }
  } else {
    for (int y = 0; y < dh; ++y) {
      const uint8_t* src = w->buffer + (sy0 + y * sh / dh) * stride + sx0;
      uint8_t* dst = fb + (dy0 + y) * kWidth + dx0;
      for (int x = 0; x < dw; ++x) dst[x] = src[x * sw / dw];
    }
  }
  video::MarkDirty();
}

extern "C" {

int32_t* __cdecl VfxDescribe() { return g_desc; }
void __cdecl VfxInit() { Log("vfx: init (640x480x256, native driver)"); }
void __cdecl VfxShutdown() { Log("vfx: shutdown"); }

void __cdecl VfxAreaWipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t color) {
  VFX_TRACE("vfx area_wipe %d,%d-%d,%d c=%d", x0, y0, x1, y1, color);
  x0 = Clamp(x0, 0, kWidth - 1); x1 = Clamp(x1, 0, kWidth - 1);
  y0 = Clamp(y0, 0, kHeight - 1); y1 = Clamp(y1, 0, kHeight - 1);
  if (x1 < x0 || y1 < y0) return;
  uint8_t* fb = video::Framebuffer();
  for (int y = y0; y <= y1; ++y) std::memset(fb + y * kWidth + x0, uint8_t(color), x1 - x0 + 1);
  video::MarkDirty();
}

void __cdecl VfxWaitVblankLeading() { video::WaitVblank(); }
void __cdecl VfxWaitVblank() { video::WaitVblank(); }

void __cdecl VfxWindowRefresh(VfxWindow* w, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  if (w == g_image->Ptr<VfxWindow>(kCursorCompWindow)) { CaptureCursor(); return; }  // draw pointer
  if (w == g_image->Ptr<VfxWindow>(kCursorBgWindow)) return;                        // erase pointer
  VFX_TRACE("vfx window_refresh %p (%dx%d) -> %d,%d-%d,%d", (void*)w->buffer, w->xMax + 1, w->yMax + 1, x0, y0, x1, y1);
  static bool warned = false;
  if (w->stencil && !warned) { warned = true; Log("vfx: note - window with stencil refreshed"); }
  Blit(w, 0, 0, w->xMax, w->yMax, x0, y0, x1, y1);
  video::Present();
}

void __cdecl VfxWindowRead(VfxWindow* w, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  if (w == g_image->Ptr<VfxWindow>(kCursorBgWindow)) return;  // pointer background: not needed
  VFX_TRACE("vfx window_read %p -> %d,%d-%d,%d", (void*)w->buffer, x0, y0, x1, y1);
  // screen rectangle -> whole window
  x0 = Clamp(x0, 0, kWidth - 1); x1 = Clamp(x1, 0, kWidth - 1);
  y0 = Clamp(y0, 0, kHeight - 1); y1 = Clamp(y1, 0, kHeight - 1);
  if (x1 < x0 || y1 < y0) return;
  const int stride = w->xMax + 1, ww = w->xMax + 1, wh = w->yMax + 1;
  const int sw = x1 - x0 + 1, sh = y1 - y0 + 1;
  const uint8_t* fb = video::Framebuffer();
  for (int y = 0; y < wh; ++y) {
    const uint8_t* src = fb + (y0 + y * sh / wh) * kWidth + x0;
    uint8_t* dst = w->buffer + y * stride;
    if (sw == ww) std::memcpy(dst, src, ww);
    else for (int x = 0; x < ww; ++x) dst[x] = src[x * sw / ww];
  }
}

void __cdecl VfxDacRead(int32_t index, uint8_t* rgb) { video::GetPalette(index, rgb, rgb + 1, rgb + 2); }
void __cdecl VfxDacWrite(int32_t index, const uint8_t* rgb) {
  VFX_TRACE("vfx dac_write %d", index);
  video::SetPalette(index, rgb[0], rgb[1], rgb[2]);
}
void __cdecl VfxBankReset() {}

void __cdecl VfxPaneRefresh(VfxPane* p, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  VFX_TRACE("vfx pane_refresh %p [%d,%d-%d,%d] -> %d,%d-%d,%d", (void*)p->window->buffer, p->x0, p->y0, p->x1, p->y1, x0, y0, x1, y1);
  const VfxWindow* w = p->window;
  int sx0 = Clamp(p->x0, 0, w->xMax), sx1 = p->x1 > w->xMax ? w->xMax : p->x1;
  int sy0 = Clamp(p->y0, 0, w->yMax), sy1 = p->y1 > w->yMax ? w->yMax : p->y1;
  if (p->x0 > w->xMax || p->y0 > w->yMax || p->x1 < 0 || p->y1 < 0) return;
  Blit(w, sx0, sy0, sx1, sy1, x0, y0, x1, y1);
  video::Present();
}

// Address of screen pixel (x,y) and how many bytes may be written from there.
void __cdecl VfxLineAddress(int32_t x, int32_t y, uint8_t** addr, int32_t* count) {
  VFX_TRACE("vfx line_address %d,%d", x, y);
  x = Clamp(x, 0, kWidth - 1);
  y = Clamp(y, 0, kHeight - 1);
  if (addr) *addr = video::Framebuffer() + y * kWidth + x;
  if (count) *count = kWidth - x;
  video::MarkDirty();
}

void* g_table[13] = {
    reinterpret_cast<void*>(VfxDescribe),        reinterpret_cast<void*>(VfxInit),
    reinterpret_cast<void*>(VfxShutdown),        reinterpret_cast<void*>(VfxAreaWipe),
    reinterpret_cast<void*>(VfxWaitVblankLeading), reinterpret_cast<void*>(VfxWaitVblank),
    reinterpret_cast<void*>(VfxWindowRefresh),   reinterpret_cast<void*>(VfxWindowRead),
    reinterpret_cast<void*>(VfxDacRead),         reinterpret_cast<void*>(VfxDacWrite),
    reinterpret_cast<void*>(VfxBankReset),       reinterpret_cast<void*>(VfxPaneRefresh),
    reinterpret_cast<void*>(VfxLineAddress),
};

// Hook for the game's "pane refresh with pointer" wrapper (0x60BF2). On DOS it
// drew the pointer into the pane before refreshing a region under it, to
// avoid flicker. With the pointer as an overlay, every refresh is plain.
// Watcom register convention: EAX pane, EDX x0, EBX y0, ECX x1, [esp+4] y1,
// callee pops 4; ESI/EDI/EBP preserved (the cdecl callee does that).
void __cdecl PaneRefreshFromGame(VfxPane* p, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  VfxPaneRefresh(p, x0, y0, x1, y1);
}
void PaneRefreshWatcomThunk();
__asm__(
    ".text\n"
    "_PaneRefreshWatcomThunk:\n"
    "  pushl 4(%esp)\n"   // y1
    "  pushl %ecx\n"      // x1
    "  pushl %ebx\n"      // y0
    "  pushl %edx\n"      // x0
    "  pushl %eax\n"      // pane
    "  call _PaneRefreshFromGame\n"
    "  addl $20, %esp\n"
    "  ret $4\n");

// Hook for VFX_register_driver (0x571F1): ignore the loaded DLL, install ours.
void __cdecl RegisterDriver(void* /*loadedDriver*/) {
  std::memcpy(g_image->Ptr<void*>(0x86A78), g_table, sizeof g_table);
  Log("vfx: native video driver registered");
}

// Hook for the game's VESA capability check (0x4F112): "mode 0x101 (640x480
// 256 colours) is supported" - so the UniVBE fallback is never touched.
int32_t __cdecl VesaModeSupported(int32_t mode) {
  Log("vesa: mode %x reported as supported", mode);
  return 0x4F;
}

}  // extern "C"

}  // namespace

bool InstallVfxDriver(GameImage& image, bool trace) {
  g_image = &image;
  g_trace = trace;
  video::SetCursorVisibilityQuery(CursorVisible);
  video::g_debugOffscreenWindowHost = uint32_t(uintptr_t(image.Ptr<void>(0xC84F8)));
  const HookSpec hooks[] = {
      {"VFX_register_driver", 0x571F1, {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x06, 0xFC}, 8,
       reinterpret_cast<void*>(RegisterDriver)},
      {"pane refresh with pointer", 0x60BF2, {0x56, 0x57, 0x55, 0x89, 0xE5, 0x81, 0xEC, 0xB0}, 8,
       reinterpret_cast<void*>(PaneRefreshWatcomThunk)},
      {"VESA mode check", 0x4F112, {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x06, 0x9C}, 8,
       reinterpret_cast<void*>(VesaModeSupported)},
  };
  for (const auto& h : hooks)
    if (!InstallHook(image, h)) return false;
  return true;
}

}  // namespace asc
