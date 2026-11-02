#include "runtime/video.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/png_write.h"
#include "runtime/dos.h"

namespace asc::video {
namespace {

Config g_cfg;
SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
uint8_t g_fb[kWidth * kHeight];
uint8_t g_pal6[256 * 3];
uint32_t g_pal32[256];
bool g_dirty = true;
uint64_t g_lastPresent = 0, g_lastShot = 0, g_lastVblank = 0;
int g_shotIndex = 0;
uint64_t NowUs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return uint64_t(c.QuadPart) * 1000000ull / uint64_t(freq.QuadPart);
}

Mouse g_mouse;
void (*g_inputListener)() = nullptr;
uint16_t g_keys[64];
int g_keyHead = 0, g_keyCount = 0;
uint64_t g_startUs = 0;

struct ScriptEvent { uint64_t atMs; std::string kind; int a = 0, b = 0; std::string key; bool done = false; };
std::vector<ScriptEvent> g_script;
int g_scriptRelease = -1;  // pending button release time (ms)

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

void RunScript() {
  if (g_script.empty()) return;
  uint64_t ms = (NowUs() - g_startUs) / 1000;
  if (g_scriptRelease >= 0 && ms >= uint64_t(g_scriptRelease)) { g_mouse.buttons = 0; g_scriptRelease = -1; }
  for (auto& e : g_script) {
    if (e.done || ms < e.atMs) continue;
    e.done = true;
    if (e.kind == "move") { g_mouse.x = e.a; g_mouse.y = e.b; }
    else if (e.kind == "click" || e.kind == "rclick") {
      g_mouse.x = e.a; g_mouse.y = e.b;
      g_mouse.buttons = e.kind == "click" ? 1 : 2;
      g_scriptRelease = int(ms + 150);
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

void ToggleFullscreen() {
  if (!g_window) return;
  bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  g_dirty = true;
}

void PumpEvents() {
  RunScript();
  if (g_cfg.headless) {
    if (g_inputListener) g_inputListener();
    return;
  }
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        Log("window closed - exiting");
        ExitProcess(0);
      case SDL_MOUSEMOTION:
        g_mouse.x = e.motion.x;
        g_mouse.y = e.motion.y;
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        if (e.type == SDL_MOUSEBUTTONDOWN) g_mouse.buttons |= bit; else g_mouse.buttons &= ~bit;
        g_mouse.x = e.button.x;
        g_mouse.y = e.button.y;
        break;
      }
      case SDL_KEYDOWN:
        if (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) { ToggleFullscreen(); break; }
        if (uint16_t k = BiosKey(e.key.keysym)) PushKey(k);
        break;
      case SDL_WINDOWEVENT:
        g_dirty = true;
        break;
      default:
        break;
    }
  }
  if (g_mouse.x < 0) g_mouse.x = 0;
  if (g_mouse.y < 0) g_mouse.y = 0;
  if (g_mouse.x >= kWidth) g_mouse.x = kWidth - 1;
  if (g_mouse.y >= kHeight) g_mouse.y = kHeight - 1;
  if (g_inputListener) g_inputListener();
}

}  // namespace

bool Init(const Config& cfg) {
  g_cfg = cfg;
  g_startUs = NowUs();
  for (int i = 0; i < 256; ++i) SetPalette(i, 0, 0, 0);
  if (cfg.headless) {
    Log("video: headless");
    return true;
  }
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.smooth ? "linear" : "nearest");
  Uint32 flags = SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  g_window = SDL_CreateWindow("Ascendancy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              kWidth * 2, kHeight * 2, flags);
  if (!g_window) { Log("SDL_CreateWindow failed: %s", SDL_GetError()); return false; }
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
  if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
  if (!g_renderer) { Log("SDL_CreateRenderer failed: %s", SDL_GetError()); return false; }
  SDL_RenderSetLogicalSize(g_renderer, kWidth, kHeight);  // 4:3, letterboxed
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                kWidth, kHeight);
  if (!g_texture) { Log("SDL_CreateTexture failed: %s", SDL_GetError()); return false; }
  SDL_ShowCursor(SDL_DISABLE);  // the game draws its own pointer
  SDL_RendererInfo info;
  SDL_GetRendererInfo(g_renderer, &info);
  Log("video: SDL %s renderer, %s", info.name, cfg.fullscreen ? "borderless fullscreen" : "windowed");
  return true;
}

void Shutdown() {
  if (g_texture) SDL_DestroyTexture(g_texture);
  if (g_renderer) SDL_DestroyRenderer(g_renderer);
  if (g_window) SDL_DestroyWindow(g_window);
  g_texture = nullptr; g_renderer = nullptr; g_window = nullptr;
  SDL_Quit();
}

uint8_t* Framebuffer() { return g_fb; }
void MarkDirty() { g_dirty = true; }

void SetPalette(int i, uint8_t r, uint8_t g, uint8_t b) {
  i &= 255;
  g_pal6[i * 3] = r & 63; g_pal6[i * 3 + 1] = g & 63; g_pal6[i * 3 + 2] = b & 63;
  auto x = [](uint8_t v) { return uint32_t((v & 63) * 255 / 63); };
  g_pal32[i] = 0xFF000000u | x(r) << 16 | x(g) << 8 | x(b);
  g_dirty = true;
}

void GetPalette(int i, uint8_t* r, uint8_t* g, uint8_t* b) {
  i &= 255;
  *r = g_pal6[i * 3]; *g = g_pal6[i * 3 + 1]; *b = g_pal6[i * 3 + 2];
}

void Present(bool force) {
  PumpEvents();
  uint64_t now = NowUs();
  if (g_cfg.screenshotEveryMs > 0 && now - g_lastShot >= uint64_t(g_cfg.screenshotEveryMs) * 1000) {
    g_lastShot = now;
    char name[64];
    std::snprintf(name, sizeof name, "\\shot_%04d.png", g_shotIndex++);
    SaveScreenshot(g_cfg.screenshotDir + name);
  }
  if (!g_dirty || (!force && now - g_lastPresent < 8000)) return;  // <= ~120 fps
  g_lastPresent = now;
  g_dirty = false;
  if (!g_renderer) return;
  void* pixels;
  int pitch;
  if (SDL_LockTexture(g_texture, nullptr, &pixels, &pitch) == 0) {
    for (int y = 0; y < kHeight; ++y) {
      uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
      const uint8_t* src = g_fb + y * kWidth;
      for (int x = 0; x < kWidth; ++x) dst[x] = g_pal32[src[x]];
    }
    SDL_UnlockTexture(g_texture);
  }
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

void WaitVblank() {
  // 60 Hz pacing: sleep until 16.67 ms after the previous retrace.
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
  return WritePngRGBA(path, kWidth, kHeight, rgba);
}

Mouse GetMouse() {
  PumpEvents();
  return g_mouse;
}

Mouse GetMouseNoPump() { return g_mouse; }

bool PeekKey(uint16_t* key) {
  PumpEvents();
  if (!g_keyCount) return false;
  *key = g_keys[g_keyHead];
  return true;
}

bool PopKey(uint16_t* key) {
  if (!PeekKey(key)) return false;
  g_keyHead = (g_keyHead + 1) % 64;
  --g_keyCount;
  return true;
}

uint8_t ShiftFlags() {
  if (g_cfg.headless) return 0;
  SDL_Keymod m = SDL_GetModState();
  return uint8_t(((m & KMOD_RSHIFT) ? 1 : 0) | ((m & KMOD_LSHIFT) ? 2 : 0) | ((m & KMOD_CTRL) ? 4 : 0) |
                 ((m & KMOD_ALT) ? 8 : 0));
}

void SetInputScript(const std::string& script) {
  size_t pos = 0;
  while (pos < script.size()) {
    size_t end = script.find(',', pos);
    std::string item = script.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? script.size() : end + 1;
    std::vector<std::string> f;
    size_t p = 0;
    while (p <= item.size()) {
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
    g_script.push_back(e);
  }
  Log("script: %zu events", g_script.size());
}
void SetInputListener(void (*fn)()) { g_inputListener = fn; }

void WarpMouse(int x, int y) {
  g_mouse.x = x;
  g_mouse.y = y;
  // The OS pointer stays where it is; the game's own pointer is what counts.
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
