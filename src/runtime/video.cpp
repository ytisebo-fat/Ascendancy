#include "runtime/video.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/png_write.h"
#include "runtime/dos.h"

namespace asc::video {
uint32_t g_debugOffscreenWindowHost = 0;
namespace {

// ---------------------------------------------------------------------------
// Shared state. The game thread writes the framebuffer/palette without locks
// (the render thread may show a half-updated frame - exactly what a VGA card
// scanning out video memory does). Input and cursor data use a lock.
// ---------------------------------------------------------------------------
Config g_cfg;
uint8_t g_fb[kWidth * kHeight];
uint8_t g_pal6[256 * 3];
uint32_t g_pal32[256];
std::atomic<bool> g_dirty{true};

CRITICAL_SECTION g_lock;   // mouse, keys, cursor image
Mouse g_mouse;
uint16_t g_keys[64];
int g_keyHead = 0, g_keyCount = 0;
uint8_t g_shift = 0;
bool g_warpPending = false;
void (*g_inputListener)() = nullptr;

struct Cursor {
  std::vector<uint8_t> pix, mask;
  int w = 0, h = 0, offX = 0, offY = 0;
};
Cursor g_cursor;
bool (*g_cursorVisible)() = nullptr;

uint64_t g_startUs = 0, g_lastShot = 0, g_lastVblank = 0;
int g_shotIndex = 0;
HANDLE g_renderThread = nullptr, g_renderReady = nullptr;
bool g_renderOk = false;

struct ScriptEvent { uint64_t atMs; std::string kind; int a = 0, b = 0; std::string key; bool done = false; };
std::vector<ScriptEvent> g_script;
int64_t g_scriptRelease = -1;

struct Lock {
  Lock() { EnterCriticalSection(&g_lock); }
  ~Lock() { LeaveCriticalSection(&g_lock); }
};

uint64_t NowUs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return uint64_t(c.QuadPart) * 1000000ull / uint64_t(freq.QuadPart);
}

// caller holds g_lock
void PushKey(uint16_t k) {
  if (g_keyCount >= 64) return;
  g_keys[(g_keyHead + g_keyCount) % 64] = k;
  ++g_keyCount;
}

// SDL scancode -> PC/XT set-1 scan code.
uint8_t ScanCode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return uint8_t(0x3B + (sc - SDL_SCANCODE_F1));
  switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_F11: return 0x85; case SDL_SCANCODE_F12: return 0x86;
    case SDL_SCANCODE_HOME: case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_END: case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_INSERT: case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_DELETE: case SDL_SCANCODE_KP_PERIOD: return 0x53;
    default: return 0;
  }
}

// Builds the BIOS key word for a key press (scan code + ASCII, with the
// usual Shift/Ctrl/Alt variants).
uint16_t BiosKey(const SDL_Keysym& ks) {
  uint8_t scan = ScanCode(ks.scancode);
  if (!scan) return 0;
  bool shift = ks.mod & KMOD_SHIFT, ctrl = ks.mod & KMOD_CTRL, alt = ks.mod & KMOD_ALT;
  SDL_Keycode k = ks.sym;
  uint8_t ascii = 0;
  if (k >= SDLK_a && k <= SDLK_z) {
    if (alt) ascii = 0;
    else if (ctrl) ascii = uint8_t(k - SDLK_a + 1);
    else ascii = uint8_t(shift ? k - 32 : k);
  } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) ascii = 13;
  else if (k == SDLK_ESCAPE) ascii = 27;
  else if (k == SDLK_BACKSPACE) ascii = 8;
  else if (k == SDLK_TAB) ascii = 9;
  else if (k >= 32 && k < 127 && !alt && !ctrl) {
    static const char kShifted[] = ")!@#$%^&*(";
    ascii = uint8_t(k);
    if (shift) {
      if (k >= '0' && k <= '9') ascii = uint8_t(kShifted[k - '0']);
      else switch (k) {
        case '-': ascii = '_'; break; case '=': ascii = '+'; break; case '[': ascii = '{'; break;
        case ']': ascii = '}'; break; case ';': ascii = ':'; break; case '\'': ascii = '"'; break;
        case ',': ascii = '<'; break; case '.': ascii = '>'; break; case '/': ascii = '?'; break;
        case '`': ascii = '~'; break; case '\\': ascii = '|'; break;
      }
    }
  }
  // Function keys with modifiers use their own scan codes.
  if (scan >= 0x3B && scan <= 0x44) {
    if (alt) scan = uint8_t(scan - 0x3B + 0x68);
    else if (ctrl) scan = uint8_t(scan - 0x3B + 0x5E);
    else if (shift) scan = uint8_t(scan - 0x3B + 0x54);
  }
  return uint16_t(scan << 8 | ascii);
}

void ClampMouse() {
  if (g_mouse.x < 0) g_mouse.x = 0;
  if (g_mouse.y < 0) g_mouse.y = 0;
  if (g_mouse.x >= kWidth) g_mouse.x = kWidth - 1;
  if (g_mouse.y >= kHeight) g_mouse.y = kHeight - 1;
}

// Applies scripted input that is due. Caller holds g_lock.
void RunScriptLocked() {
  if (g_script.empty()) return;
  int64_t ms = int64_t((NowUs() - g_startUs) / 1000);
  if (g_scriptRelease >= 0 && ms >= g_scriptRelease) { g_mouse.buttons = 0; g_scriptRelease = -1; }
  for (auto& e : g_script) {
    if (e.done || ms < int64_t(e.atMs)) continue;
    e.done = true;
    if (e.kind == "move") { g_mouse.x = e.a; g_mouse.y = e.b; }
    else if (e.kind == "click" || e.kind == "rclick") {
      g_mouse.x = e.a; g_mouse.y = e.b;
      g_mouse.buttons = e.kind == "click" ? 1 : 2;
      g_scriptRelease = ms + 150;
    } else if (e.kind == "key") {
      static const struct { const char* n; uint16_t k; } kNamed[] = {
          {"esc", 0x011B}, {"enter", 0x1C0D}, {"space", 0x3920}, {"tab", 0x0F09},
          {"up", 0x4800}, {"down", 0x5000}, {"left", 0x4B00}, {"right", 0x4D00}};
      for (auto& n : kNamed) if (e.key == n.n) PushKey(n.k);
      if (e.key.size() == 1) {
        SDL_Keysym ks{};
        ks.sym = SDL_Keycode(e.key[0]);
        ks.scancode = SDL_GetScancodeFromKey(ks.sym);
        if (uint16_t k = BiosKey(ks)) PushKey(k);
      }
    } else if (e.kind == "watchoff") {  // debug: watch offscreen pixel (a,b) of window at 0xC84F8
      uint32_t buf = *reinterpret_cast<uint32_t*>(uintptr_t(g_debugOffscreenWindowHost));
      DosSetWatch(buf + uint32_t(e.b) * 640 + uint32_t(e.a));
    } else if (e.kind == "fbshot") {  // framebuffer only, no pointer overlay
      bool (*saved)() = g_cursorVisible;
      g_cursorVisible = nullptr;
      char name[64];
      std::snprintf(name, sizeof name, "\\fb_%05llu.png", (unsigned long long)e.atMs);
      SaveScreenshot(g_cfg.screenshotDir + name);
      g_cursorVisible = saved;
    } else if (e.kind == "shot") {
      char name[64];
      std::snprintf(name, sizeof name, "\\script_%05llu.png", (unsigned long long)e.atMs);
      SaveScreenshot(g_cfg.screenshotDir + name);
    } else if (e.kind == "quit") {
      Log("script: quit");
      ExitProcess(0);
    }
  }
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;

bool confineDirty = true;

// In fullscreen the 4:3 picture has black bars at the sides. Keep the OS
// mouse inside the picture: otherwise it can wander into a bar while the game
// pointer stays clamped at the edge, and seems to stick there when moving back.
// (Windowed mode leaves the mouse free so it can reach other windows.)
void UpdateMouseConfinement() {
  bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
  if (!fs) {
    SDL_SetWindowMouseRect(g_window, nullptr);
    return;
  }
  int x0, y0, x1, y1;
  SDL_RenderLogicalToWindow(g_renderer, 0.0f, 0.0f, &x0, &y0);
  SDL_RenderLogicalToWindow(g_renderer, float(kWidth), float(kHeight), &x1, &y1);
  SDL_Rect r{x0, y0, x1 - x0, y1 - y0};
  if (r.w > 0 && r.h > 0) SDL_SetWindowMouseRect(g_window, &r);
}

void ToggleFullscreen() {
  bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  confineDirty = true;
}

void HandleEvents() {
  SDL_Event e;
  bool any = false;
  Lock lock;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        Log("window closed - exiting");
        ExitProcess(0);
      case SDL_MOUSEMOTION:
        g_mouse.x = e.motion.x;
        g_mouse.y = e.motion.y;
        any = true;
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        if (e.type == SDL_MOUSEBUTTONDOWN) g_mouse.buttons |= bit; else g_mouse.buttons &= ~bit;
        g_mouse.x = e.button.x;
        g_mouse.y = e.button.y;
        any = true;
        break;
      }
      case SDL_KEYDOWN:
        if (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) { ToggleFullscreen(); break; }
        if (uint16_t k = BiosKey(e.key.keysym)) PushKey(k);
        break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
          confineDirty = true;
        break;
      default:
        break;
    }
  }
  if (confineDirty) {
    confineDirty = false;
    UpdateMouseConfinement();
  }
  SDL_Keymod m = SDL_GetModState();
  g_shift = uint8_t(((m & KMOD_RSHIFT) ? 1 : 0) | ((m & KMOD_LSHIFT) ? 2 : 0) |
                    ((m & KMOD_CTRL) ? 4 : 0) | ((m & KMOD_ALT) ? 8 : 0));
  RunScriptLocked();
  ClampMouse();
  if (g_warpPending) {
    g_warpPending = false;
    int wx, wy;
    SDL_RenderLogicalToWindow(g_renderer, float(g_mouse.x), float(g_mouse.y), &wx, &wy);
    SDL_WarpMouseInWindow(g_window, wx, wy);
  }
  (void)any;
  if (g_inputListener) g_inputListener();
}

void DrawFrame() {
  void* pixels;
  int pitch;
  if (SDL_LockTexture(g_texture, nullptr, &pixels, &pitch) != 0) return;
  for (int y = 0; y < kHeight; ++y) {
    uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
    const uint8_t* src = g_fb + y * kWidth;
    for (int x = 0; x < kWidth; ++x) dst[x] = g_pal32[src[x]];
  }
  // Mouse pointer overlay at the current mouse position.
  if (g_cursorVisible && g_cursorVisible()) {
    Lock lock;
    const Cursor& c = g_cursor;
    int x0 = g_mouse.x + c.offX, y0 = g_mouse.y + c.offY;
    for (int y = 0; y < c.h; ++y) {
      int sy = y0 + y;
      if (sy < 0 || sy >= kHeight) continue;
      uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + sy * pitch);
      for (int x = 0; x < c.w; ++x) {
        int sx = x0 + x;
        if (sx < 0 || sx >= kWidth || !c.mask[y * c.w + x]) continue;
        dst[sx] = g_pal32[c.pix[y * c.w + x]];
      }
    }
  }
  SDL_UnlockTexture(g_texture);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

DWORD WINAPI RenderThread(void*) {
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    Log("SDL_Init failed: %s", SDL_GetError());
    SetEvent(g_renderReady);
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_cfg.smooth ? "linear" : "nearest");
  Uint32 flags = SDL_WINDOW_RESIZABLE | (g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  g_window = SDL_CreateWindow("Ascendancy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              kWidth * 2, kHeight * 2, flags);
  if (g_window) {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (g_renderer) {
    SDL_RenderSetLogicalSize(g_renderer, kWidth, kHeight);  // 4:3, letterboxed
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  kWidth, kHeight);
  }
  if (!g_texture) {
    Log("could not create the game window: %s", SDL_GetError());
    SetEvent(g_renderReady);
    return 1;
  }
  SDL_ShowCursor(SDL_DISABLE);  // the pointer is drawn as an overlay
  SDL_RendererInfo info;
  SDL_GetRendererInfo(g_renderer, &info);
  bool vsync = info.flags & SDL_RENDERER_PRESENTVSYNC;
  Log("video: SDL %s renderer%s, %s", info.name, vsync ? " (vsync)" : "",
      g_cfg.fullscreen ? "borderless fullscreen" : "windowed");
  g_renderOk = true;
  SetEvent(g_renderReady);
  for (;;) {
    uint64_t t0 = NowUs();
    HandleEvents();
    DrawFrame();
    if (!vsync) {  // ~120 fps cap without vsync
      uint64_t dt = NowUs() - t0;
      if (dt < 8000) Sleep(DWORD((8000 - dt) / 1000));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// API (game thread)
// ---------------------------------------------------------------------------
bool Init(const Config& cfg) {
  g_cfg = cfg;
  g_startUs = NowUs();
  InitializeCriticalSection(&g_lock);
  for (int i = 0; i < 256; ++i) SetPalette(i, 0, 0, 0);
  if (cfg.headless) {
    Log("video: headless");
    return true;
  }
  g_renderReady = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_renderThread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
  WaitForSingleObject(g_renderReady, INFINITE);
  return g_renderOk;
}

void Shutdown() {}

uint8_t* Framebuffer() { return g_fb; }
void MarkDirty() { g_dirty = true; }

void SetPalette(int i, uint8_t r, uint8_t g, uint8_t b) {
  i &= 255;
  g_pal6[i * 3] = r & 63; g_pal6[i * 3 + 1] = g & 63; g_pal6[i * 3 + 2] = b & 63;
  auto x = [](uint8_t v) { return uint32_t((v & 63) * 255 / 63); };
  g_pal32[i] = 0xFF000000u | x(r) << 16 | x(g) << 8 | x(b);
}

void GetPalette(int i, uint8_t* r, uint8_t* g, uint8_t* b) {
  i &= 255;
  *r = g_pal6[i * 3]; *g = g_pal6[i * 3 + 1]; *b = g_pal6[i * 3 + 2];
}

void Present(bool) {
  uint64_t now = NowUs();
  if (g_cfg.headless) {
    Lock lock;
    RunScriptLocked();
    ClampMouse();
    if (g_inputListener) g_inputListener();
  }
  if (g_cfg.screenshotEveryMs > 0 && now - g_lastShot >= uint64_t(g_cfg.screenshotEveryMs) * 1000) {
    g_lastShot = now;
    char name[64];
    std::snprintf(name, sizeof name, "\\shot_%04d.png", g_shotIndex++);
    SaveScreenshot(g_cfg.screenshotDir + name);
  }
}

void WaitVblank() {
  uint64_t now = NowUs();
  uint64_t next = g_lastVblank + 16667;
  if (now < next && next - now < 20000) {
    if (next - now > 2000) Sleep(DWORD((next - now) / 1000 - 1));
    while (NowUs() < next) {}
    now = next;
  }
  g_lastVblank = now;
  Present(true);
}

bool SaveScreenshot(const std::string& path) {
  static uint8_t rgba[kWidth * kHeight * 4];
  for (int i = 0; i < kWidth * kHeight; ++i) {
    uint32_t c = g_pal32[g_fb[i]];
    rgba[i * 4] = uint8_t(c >> 16); rgba[i * 4 + 1] = uint8_t(c >> 8);
    rgba[i * 4 + 2] = uint8_t(c); rgba[i * 4 + 3] = 255;
  }
  // include the pointer overlay, as the player sees it
  if (g_cursorVisible && g_cursorVisible()) {
    Lock lock;
    const Cursor& c = g_cursor;
    for (int y = 0; y < c.h; ++y)
      for (int x = 0; x < c.w; ++x) {
        int sx = g_mouse.x + c.offX + x, sy = g_mouse.y + c.offY + y;
        if (sx < 0 || sy < 0 || sx >= kWidth || sy >= kHeight || !c.mask[y * c.w + x]) continue;
        uint32_t col = g_pal32[c.pix[y * c.w + x]];
        uint8_t* p = rgba + (sy * kWidth + sx) * 4;
        p[0] = uint8_t(col >> 16); p[1] = uint8_t(col >> 8); p[2] = uint8_t(col);
      }
  }
  return WritePngRGBA(path, kWidth, kHeight, rgba);
}

Mouse GetMouse() {
  if (g_cfg.headless) Present();
  Lock lock;
  return g_mouse;
}

// Called from the input listener (lock already held by the caller thread).
Mouse GetMouseNoPump() { return g_mouse; }

void SetInputListener(void (*fn)()) { g_inputListener = fn; }

void WarpMouse(int x, int y) {
  Lock lock;
  g_mouse.x = x;
  g_mouse.y = y;
  ClampMouse();
  g_warpPending = !g_cfg.headless;
}

bool PeekKey(uint16_t* key) {
  if (g_cfg.headless) Present();
  Lock lock;
  if (!g_keyCount) return false;
  *key = g_keys[g_keyHead];
  return true;
}

bool PopKey(uint16_t* key) {
  if (g_cfg.headless) Present();
  Lock lock;
  if (!g_keyCount) return false;
  *key = g_keys[g_keyHead];
  g_keyHead = (g_keyHead + 1) % 64;
  --g_keyCount;
  return true;
}

uint8_t ShiftFlags() { return g_shift; }

void SetCursorImage(const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& mask, int w,
                    int h, int offX, int offY) {
  Lock lock;
  g_cursor.pix = pixels;
  g_cursor.mask = mask;
  g_cursor.w = w;
  g_cursor.h = h;
  g_cursor.offX = offX;
  g_cursor.offY = offY;
}

void SetCursorVisibilityQuery(bool (*fn)()) { g_cursorVisible = fn; }

void SetInputScript(const std::string& script) {
  size_t pos = 0;
  while (pos < script.size()) {
    size_t end = script.find(',', pos);
    std::string item = script.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? script.size() : end + 1;
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
      size_t c = item.find(':', p);
      f.push_back(item.substr(p, c == std::string::npos ? std::string::npos : c - p));
      if (c == std::string::npos) break;
      p = c + 1;
    }
    if (f.size() < 2) continue;
    ScriptEvent e;
    e.atMs = std::strtoull(f[0].c_str(), nullptr, 10);
    e.kind = f[1];
    if (f.size() > 2) { e.a = std::atoi(f[2].c_str()); e.key = f[2]; }
    if (f.size() > 3) e.b = std::atoi(f[3].c_str());
    Lock lock;
    g_script.push_back(e);
  }
  Log("script: %zu events", g_script.size());
}

}  // namespace asc::video

// The prebuilt SDL2 MinGW library was compiled with GCC and calls libgcc's
// stack probe, which llvm-mingw does not provide. Same contract as libgcc:
// EAX = bytes to probe, touches each page below ESP, preserves all registers.
__asm__(
    ".globl ___chkstk_ms\n"
    "___chkstk_ms:\n"
    "  pushl %ecx\n"
    "  pushl %eax\n"
    "  cmpl $0x1000, %eax\n"
    "  leal 12(%esp), %ecx\n"
    "  jb 2f\n"
    "1:\n"
    "  subl $0x1000, %ecx\n"
    "  orl $0, (%ecx)\n"
    "  subl $0x1000, %eax\n"
    "  cmpl $0x1000, %eax\n"
    "  ja 1b\n"
    "2:\n"
    "  subl %eax, %ecx\n"
    "  orl $0, (%ecx)\n"
    "  popl %eax\n"
    "  popl %ecx\n"
    "  ret\n");
