#include "runtime/video.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#include "assets/font.h"
#include "common/png_write.h"
#include "runtime/dos.h"
#include "runtime/options_ui.h"

namespace asc::video {
uint32_t g_debugOffscreenWindowHost = 0;
uint32_t (*g_debugPeek)(uint32_t origAddr) = nullptr;
void (*g_debugDump)(const char* path) = nullptr;
namespace {

// ---------------------------------------------------------------------------
// Shared state. The game thread writes the framebuffer/palette without locks
// (the render thread may show a half-updated frame - exactly what a VGA card
// scanning out video memory does). Input, cursor and settings use a lock.
// ---------------------------------------------------------------------------
Config g_cfg;
Settings g_settings;
uint8_t g_fb[kWidth * kHeight];
uint8_t g_pal6[256 * 3];
uint32_t g_pal32[256];

CRITICAL_SECTION g_lock;
// Initialised at start-up so the API is usable before Init().
struct LockInit { LockInit() { InitializeCriticalSection(&g_lock); } } g_lockInit;
Mouse g_mouse;            // what the game sees
uint16_t g_keys[64];
int g_keyHead = 0, g_keyCount = 0;
uint8_t g_shift = 0;
uint8_t g_scriptShift = 0;
uint64_t g_helpShiftUntil = 0;
bool g_warpPending = false;
void (*g_inputListener)() = nullptr;

struct Cursor {
  std::vector<uint8_t> pix, mask;
  int w = 0, h = 0, offX = 0, offY = 0;
  int version = 0;
  int shape = -1;
};
constexpr int kHelpPointerShape = 1;  // arrow with "HELP", shown while Shift is held
bool g_realShift = false;             // Shift physically held
Cursor g_cursor;
Cursor g_arrow;           // the game's arrow pointer, used over the options panel
bool (*g_cursorVisible)() = nullptr;
int (*g_screenQuery)() = nullptr;

uint64_t g_startUs = 0, g_lastShot = 0, g_lastVblank = 0;
int g_shotIndex = 0;
HANDLE g_renderThread = nullptr, g_renderReady = nullptr;
bool g_renderOk = false;

struct ScriptEvent { uint64_t atMs; std::string kind; int a = 0, b = 0; std::string key; bool done = false; };
std::vector<ScriptEvent> g_script;
int64_t g_scriptRelease = -1;
int g_scriptReleaseButton = 0;
std::string g_winshotPath;

// Camera controls (wheel / middle-button drag) for 3D views
struct { int x0 = 0, y0 = 0, x1 = -1, y1 = -1, screen = -1; } g_camArea;
CameraInput g_camInput;
bool g_midDown = false, g_midMoved = false;
int g_midX = 0, g_midY = 0, g_midTravel = 0;
constexpr float kDegreesPerPixel = 0.5f;
constexpr int kZoomStepsPerNotch = 2;

// Options panel (render thread)
FntFont g_bigFont, g_smallFont;
ui::OptionsPanel g_panel;
bool g_uiDirty = true;
bool g_menuButtonHover = false;
bool g_swallowRelease = false;   // the press was consumed by the port's UI

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

// The screen number is also 0 before the game has initialised its window
// manager (start-up), so the main menu only counts once another screen (the
// logo, 0xFFFF) has been seen.
bool OnMainMenu() {
  static bool started = false;
  if (!g_screenQuery) return false;
  int screen = g_screenQuery();
  if (screen != 0) started = true;
  return started && screen == 0;
}

void SaveSettings() {
  if (!g_cfg.settingsPath.empty()) g_settings.Save(g_cfg.settingsPath);
}

// Forward declarations (render thread)
void ApplyFullscreen();

bool InCameraArea(int x, int y) {
  return !g_panel.open() && g_screenQuery && g_screenQuery() == g_camArea.screen && x >= g_camArea.x0 &&
         y >= g_camArea.y0 && x <= g_camArea.x1 && y <= g_camArea.y1;
}

// Caller holds g_lock.
void OnWheel(int notches) {
  if (!InCameraArea(g_mouse.x, g_mouse.y)) return;
  g_camInput.zoomSteps -= notches * kZoomStepsPerNotch;  // wheel up = closer
}

// Middle button: drag rotates (left/right) and tilts (up/down); a click
// without dragging re-centres. Caller holds g_lock.
void OnMiddle(bool down) {
  if (down) {
    if (!InCameraArea(g_mouse.x, g_mouse.y)) return;
    g_midDown = true;
    g_midMoved = false;
    g_midTravel = 0;
    g_midX = g_mouse.x;
    g_midY = g_mouse.y;
  } else if (g_midDown) {
    g_midDown = false;
    if (!g_midMoved) g_camInput.recenter = true;
  }
}

void OnMiddleDrag() {
  if (!g_midDown) return;
  int dx = g_mouse.x - g_midX, dy = g_mouse.y - g_midY;
  g_midX = g_mouse.x;
  g_midY = g_mouse.y;
  g_midTravel += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
  if (g_midTravel > 3) g_midMoved = true;
  g_camInput.yawDegrees += dx * kDegreesPerPixel;
  g_camInput.tiltDegrees += dy * kDegreesPerPixel;
}

// ---------------------------------------------------------------------------
// Input routing, shared by real events and scripted input. Caller holds g_lock.
// The port's UI (Options button / panel) gets first pick; everything else
// goes to the game.
// ---------------------------------------------------------------------------
void OnPointerMove(int x, int y) {
  g_mouse.x = x;
  g_mouse.y = y;
  ClampMouse();
  OnMiddleDrag();
  if (g_panel.open()) {
    if (g_panel.OnMove(g_mouse.x, g_mouse.y)) g_uiDirty = true;
  } else {
    bool h = OnMainMenu() && ui::OptionsPanel::MenuButton().Contains(g_mouse.x, g_mouse.y);
    if (h != g_menuButtonHover) { g_menuButtonHover = h; g_uiDirty = true; }
  }
}

void OnPointerButton(bool down, int sdlButton) {
  if (sdlButton == SDL_BUTTON_MIDDLE) { OnMiddle(down); return; }
  if (down && sdlButton == SDL_BUTTON_LEFT) {
    if (g_panel.open()) {
      ui::Action a = g_panel.OnClick(g_mouse.x, g_mouse.y, g_settings);
      if (a == ui::Action::kClose) g_panel.Close();
      if (a == ui::Action::kChanged) { SaveSettings(); ApplyFullscreen(); }
      g_uiDirty = true;
      g_swallowRelease = true;
      return;
    }
    if (OnMainMenu() && ui::OptionsPanel::MenuButton().Contains(g_mouse.x, g_mouse.y) && g_panel.fontsOk()) {
      g_panel.Open();
      g_mouse.buttons = 0;
      g_uiDirty = true;
      g_swallowRelease = true;
      return;
    }
  }
  if (g_panel.open()) return;  // nothing reaches the game while the panel is open
  if (!down && g_swallowRelease) { g_swallowRelease = false; return; }
  int bit = sdlButton == SDL_BUTTON_LEFT ? 1 : sdlButton == SDL_BUTTON_RIGHT ? 2 : 0;
  if (bit == 2 && g_settings.rightClickHelp) {
    // Right-click = the game's "help-click" (Shift + left click): report the
    // left button, with Shift held until shortly after release.
    bit = 1;
    g_helpShiftUntil = NowUs() + (down ? 3600000000ull : 200000);
  }
  if (down) g_mouse.buttons |= bit; else g_mouse.buttons &= ~bit;
}

// Returns true if the key was used by the port.
bool OnKeyDown(const SDL_Keysym& ks) {
  if (ks.sym == SDLK_RETURN && (ks.mod & KMOD_ALT)) {
    g_settings.fullscreen = !g_settings.fullscreen;
    SaveSettings();
    ApplyFullscreen();
    g_uiDirty = true;
    return true;
  }
  if (ks.sym == SDLK_F12 && g_panel.fontsOk()) {
    if (g_panel.open()) g_panel.Close(); else { g_panel.Open(); g_mouse.buttons = 0; }
    g_uiDirty = true;
    return true;
  }
  if (g_panel.open()) {
    if (ks.sym == SDLK_ESCAPE) { g_panel.Close(); g_uiDirty = true; }
    return true;  // the game gets no keys while the panel is open
  }
  if (uint16_t k = BiosKey(ks)) PushKey(k);
  return false;
}

// Applies scripted input that is due. Caller holds g_lock.
void RunScriptLocked() {
  if (g_script.empty()) return;
  int64_t ms = int64_t((NowUs() - g_startUs) / 1000);
  if (g_scriptRelease >= 0 && ms >= g_scriptRelease) {
    OnPointerButton(false, g_scriptReleaseButton);
    g_scriptRelease = -1;
  }
  if (g_scriptShift && g_scriptRelease < 0) {
    static int64_t clearAt = -1;
    if (clearAt < 0) clearAt = ms + 200;
    if (ms >= clearAt) { g_scriptShift = 0; clearAt = -1; }
  }
  for (auto& e : g_script) {
    if (e.done || ms < int64_t(e.atMs)) continue;
    e.done = true;
    if (e.kind == "move") {
      OnPointerMove(e.a, e.b);
    } else if (e.kind == "click" || e.kind == "rclick" || e.kind == "sclick") {
      OnPointerMove(e.a, e.b);
      if (e.kind == "sclick") g_scriptShift = 2;
      g_scriptReleaseButton = e.kind == "rclick" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
      OnPointerButton(true, g_scriptReleaseButton);
      g_scriptRelease = ms + 150;
    } else if (e.kind == "key") {
      static const struct { const char* n; SDL_Keycode k; } kNamed[] = {
          {"esc", SDLK_ESCAPE}, {"enter", SDLK_RETURN}, {"space", SDLK_SPACE}, {"tab", SDLK_TAB},
          {"up", SDLK_UP}, {"down", SDLK_DOWN}, {"left", SDLK_LEFT}, {"right", SDLK_RIGHT},
          {"f12", SDLK_F12}};
      SDL_Keysym ks{};
      ks.sym = 0;
      for (auto& n : kNamed) if (e.key == n.n) ks.sym = n.k;
      if (!ks.sym && e.key.size() == 1) ks.sym = SDL_Keycode(e.key[0]);
      if (ks.sym) {
        ks.scancode = SDL_GetScancodeFromKey(ks.sym);
        OnKeyDown(ks);
      }
    } else if (e.kind == "shot") {
      char name[64];
      std::snprintf(name, sizeof name, "\\script_%05llu.png", (unsigned long long)e.atMs);
      SaveScreenshot(g_cfg.screenshotDir + name);
    } else if (e.kind == "winshot") {  // the rendered window, as the player sees it
      char name[64];
      std::snprintf(name, sizeof name, "\\win_%05llu.png", (unsigned long long)e.atMs);
      g_winshotPath = g_cfg.screenshotDir + name;
    } else if (e.kind == "wheel") {  // wheel:<notches> (+ = up/closer)
      OnWheel(e.a);
    } else if (e.kind == "mdown") {
      OnMiddle(true);
    } else if (e.kind == "mup") {
      OnMiddle(false);
    } else if (e.kind == "dump") {  // debug: write the game's data object to a file
      char name[64];
      std::snprintf(name, sizeof name, "\\dump_%05llu.bin", (unsigned long long)e.atMs);
      if (g_debugDump) g_debugDump((g_cfg.screenshotDir + name).c_str());
    } else if (e.kind == "peek") {
      uint32_t a = uint32_t(std::strtoul(e.key.c_str(), nullptr, 16));
      if (g_debugPeek) Log("peek %08x = %08x", a, g_debugPeek(a));
    } else if (e.kind == "watchoff") {
      uint32_t buf = *reinterpret_cast<uint32_t*>(uintptr_t(g_debugOffscreenWindowHost));
      DosSetWatch(buf + uint32_t(e.b) * 640 + uint32_t(e.a));
    } else if (e.kind == "fbshot") {
      bool (*saved)() = g_cursorVisible;
      g_cursorVisible = nullptr;
      char name[64];
      std::snprintf(name, sizeof name, "\\fb_%05llu.png", (unsigned long long)e.atMs);
      SaveScreenshot(g_cfg.screenshotDir + name);
      g_cursorVisible = saved;
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
SDL_Texture* g_tex = nullptr;      // 640x480 game image
SDL_Texture* g_texBig = nullptr;   // 1280x960 Scale2x image
SDL_Texture* g_target = nullptr;   // integer pre-scale (Crisp / CRT)
int g_targetScale = 0;
SDL_Texture* g_scan = nullptr;     // scanline overlay
SDL_Texture* g_uiTex = nullptr;    // options panel / button layer
SDL_Texture* g_ptrTex = nullptr;
int g_ptrVersion = -1, g_ptrW = 0, g_ptrH = 0, g_ptrOffX = 0, g_ptrOffY = 0;
bool g_ptrIsArrow = false;
SDL_Rect g_dst{0, 0, kWidth, kHeight};   // picture area in output pixels
int g_outW = kWidth, g_outH = kHeight, g_winW = kWidth, g_winH = kHeight;
uint8_t g_lastFb[kWidth * kHeight];
uint32_t g_lastPal[256];
bool g_imageDirty = true;
Filter g_builtFilter = Filter::kCount;
uint32_t g_uiPixels[kWidth * kHeight];
bool g_uiVisible = false;
bool g_confineDirty = true;

void ApplyFullscreen() {
  if (!g_window) return;
  bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
  if (fs != g_settings.fullscreen)
    SDL_SetWindowFullscreen(g_window, g_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  g_confineDirty = true;
  g_imageDirty = true;
}

// 4:3 picture, as large as fits, centred (black bars elsewhere).
void UpdateLayout() {
  SDL_GetRendererOutputSize(g_renderer, &g_outW, &g_outH);
  SDL_GetWindowSize(g_window, &g_winW, &g_winH);
  int w = g_outW, h = g_outW * 3 / 4;
  if (h > g_outH) { h = g_outH; w = g_outH * 4 / 3; }
  g_dst = {(g_outW - w) / 2, (g_outH - h) / 2, w, h};
}

// Window coordinates (mouse events) -> game coordinates.
void WindowToGame(int wx, int wy, int* gx, int* gy) {
  double px = double(wx) * g_outW / (g_winW ? g_winW : 1);
  double py = double(wy) * g_outH / (g_winH ? g_winH : 1);
  *gx = int((px - g_dst.x) * kWidth / (g_dst.w ? g_dst.w : 1));
  *gy = int((py - g_dst.y) * kHeight / (g_dst.h ? g_dst.h : 1));
}

void GameToWindow(int gx, int gy, int* wx, int* wy) {
  double px = g_dst.x + (gx + 0.5) * g_dst.w / kWidth;
  double py = g_dst.y + (gy + 0.5) * g_dst.h / kHeight;
  *wx = int(px * g_winW / (g_outW ? g_outW : 1));
  *wy = int(py * g_winH / (g_outH ? g_outH : 1));
}

// In fullscreen keep the OS mouse inside the picture: otherwise it can
// wander into a black bar while the pointer stays clamped at the edge, and
// seems to stick there when moving back. Windowed mode leaves it free.
void UpdateMouseConfinement() {
  if (!(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
    SDL_SetWindowMouseRect(g_window, nullptr);
    return;
  }
  int x0, y0, x1, y1;
  GameToWindow(0, 0, &x0, &y0);
  GameToWindow(kWidth - 1, kHeight - 1, &x1, &y1);
  SDL_Rect r{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
  if (r.w > 0 && r.h > 0) SDL_SetWindowMouseRect(g_window, &r);
}

void HandleEvents() {
  SDL_Event e;
  Lock lock;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        Log("window closed - exiting");
        ExitProcess(0);
      case SDL_MOUSEMOTION: {
        int gx, gy;
        WindowToGame(e.motion.x, e.motion.y, &gx, &gy);
        OnPointerMove(gx, gy);
        break;
      }
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        int gx, gy;
        WindowToGame(e.button.x, e.button.y, &gx, &gy);
        OnPointerMove(gx, gy);
        OnPointerButton(e.type == SDL_MOUSEBUTTONDOWN, e.button.button);
        break;
      }
      case SDL_KEYDOWN:
        OnKeyDown(e.key.keysym);
        break;
      case SDL_MOUSEWHEEL:
        OnWheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
        break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
          g_confineDirty = true;
          g_imageDirty = true;
        }
        break;
      case SDL_RENDER_TARGETS_RESET:
      case SDL_RENDER_DEVICE_RESET:
        g_imageDirty = true;
        g_targetScale = 0;
        break;
      default:
        break;
    }
  }
  SDL_Keymod m = SDL_GetModState();
  g_shift = uint8_t(((m & KMOD_RSHIFT) ? 1 : 0) | ((m & KMOD_LSHIFT) ? 2 : 0) |
                    ((m & KMOD_CTRL) ? 4 : 0) | ((m & KMOD_ALT) ? 8 : 0));
  g_realShift = (m & KMOD_SHIFT) != 0;
  if (NowUs() < g_helpShiftUntil) g_shift |= 2;
  RunScriptLocked();
  ClampMouse();
  // The Options button follows the game's screen.
  bool uiVisible = g_panel.open() || OnMainMenu();
  if (uiVisible != g_uiVisible) { g_uiVisible = uiVisible; g_uiDirty = true; }
  if (g_warpPending) {
    g_warpPending = false;
    int wx, wy;
    GameToWindow(g_mouse.x, g_mouse.y, &wx, &wy);
    SDL_WarpMouseInWindow(g_window, wx, wy);
  }
  if (g_inputListener) g_inputListener();
}

// Scale2x (AdvMAME2x) on palette indices: exact comparisons, so it rounds
// off the stair-steps of pixel art without blurring.
void Scale2x(uint32_t* dst, int pitchPx) {
  for (int y = 0; y < kHeight; ++y) {
    const uint8_t* r0 = g_fb + (y > 0 ? y - 1 : y) * kWidth;
    const uint8_t* r1 = g_fb + y * kWidth;
    const uint8_t* r2 = g_fb + (y < kHeight - 1 ? y + 1 : y) * kWidth;
    uint32_t* d0 = dst + (2 * y) * pitchPx;
    uint32_t* d1 = d0 + pitchPx;
    for (int x = 0; x < kWidth; ++x) {
      int xl = x > 0 ? x - 1 : x, xr = x < kWidth - 1 ? x + 1 : x;
      uint8_t B = r0[x], D = r1[xl], E = r1[x], F = r1[xr], H = r2[x];
      uint8_t e0 = E, e1 = E, e2 = E, e3 = E;
      if (B != H && D != F) {
        if (D == B) e0 = D;
        if (B == F) e1 = F;
        if (D == H) e2 = D;
        if (H == F) e3 = F;
      }
      d0[2 * x] = g_pal32[e0]; d0[2 * x + 1] = g_pal32[e1];
      d1[2 * x] = g_pal32[e2]; d1[2 * x + 1] = g_pal32[e3];
    }
  }
}

void UploadGameImage() {
  void* pixels;
  int pitch;
  if (g_settings.filter == Filter::kSmooth) {
    if (!g_texBig) {
      g_texBig = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   kWidth * 2, kHeight * 2);
      SDL_SetTextureScaleMode(g_texBig, SDL_ScaleModeLinear);
    }
    if (g_texBig && SDL_LockTexture(g_texBig, nullptr, &pixels, &pitch) == 0) {
      Scale2x(static_cast<uint32_t*>(pixels), pitch / 4);
      SDL_UnlockTexture(g_texBig);
    }
    return;
  }
  if (SDL_LockTexture(g_tex, nullptr, &pixels, &pitch) != 0) return;
  for (int y = 0; y < kHeight; ++y) {
    uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
    const uint8_t* src = g_fb + y * kWidth;
    for (int x = 0; x < kWidth; ++x) dst[x] = g_pal32[src[x]];
  }
  SDL_UnlockTexture(g_tex);
}

void DrawGameImage() {
  switch (g_settings.filter) {
    case Filter::kSharp:
      SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeNearest);
      SDL_RenderCopy(g_renderer, g_tex, nullptr, &g_dst);
      break;
    case Filter::kSoft:
      SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeLinear);
      SDL_RenderCopy(g_renderer, g_tex, nullptr, &g_dst);
      break;
    case Filter::kSmooth:
      if (g_texBig) SDL_RenderCopy(g_renderer, g_texBig, nullptr, &g_dst);
      break;
    case Filter::kCrisp:
    case Filter::kCrt: {
      // "Sharp bilinear": enlarge by a whole number with hard pixels, then
      // make the last, fractional step smoothly - sharp and even pixels.
      int scale = g_dst.h / kHeight;
      if (scale < 1) scale = 1;
      if (scale != g_targetScale || !g_target) {
        if (g_target) SDL_DestroyTexture(g_target);
        g_target = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                     kWidth * scale, kHeight * scale);
        if (g_target) SDL_SetTextureScaleMode(g_target, SDL_ScaleModeLinear);
        g_targetScale = scale;
        g_imageDirty = true;
      }
      if (!g_target) {  // no render-target support: fall back to soft
        SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeLinear);
        SDL_RenderCopy(g_renderer, g_tex, nullptr, &g_dst);
        break;
      }
      if (g_imageDirty) {
        SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeNearest);
        SDL_SetRenderTarget(g_renderer, g_target);
        SDL_RenderCopy(g_renderer, g_tex, nullptr, nullptr);
        SDL_SetRenderTarget(g_renderer, nullptr);
      }
      SDL_RenderCopy(g_renderer, g_target, nullptr, &g_dst);
      if (g_settings.filter == Filter::kCrt) {
        // Scanlines generated at the exact output height: each game line
        // gets a bright centre and a darker gap (smooth cosine profile).
        static int scanH = 0;
        if (!g_scan || scanH != g_dst.h) {
          if (g_scan) SDL_DestroyTexture(g_scan);
          g_scan = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, g_dst.h);
          scanH = g_dst.h;
          if (g_scan) {
            std::vector<uint32_t> rows(size_t(g_dst.h));
            for (int y = 0; y < g_dst.h; ++y) {
              double phase = (y + 0.5) * kHeight / g_dst.h;
              phase -= int(phase);                        // 0..1 within a game line
              // bright beam in the upper part of the line, dark gap below
              double beam = std::exp(-((phase - 0.4) / 0.3) * ((phase - 0.4) / 0.3));
              int a = int((1.0 - beam) * 170);            // gap darkness
              rows[size_t(y)] = uint32_t(a) << 24;
            }
            SDL_UpdateTexture(g_scan, nullptr, rows.data(), 4);
            SDL_SetTextureBlendMode(g_scan, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_scan, SDL_ScaleModeNearest);
          }
        }
        if (g_scan) SDL_RenderCopy(g_renderer, g_scan, nullptr, &g_dst);
      }
      break;
    }
    default:
      break;
  }
}

SDL_Rect ToOutput(int gx, int gy, int gw, int gh) {
  return {g_dst.x + gx * g_dst.w / kWidth, g_dst.y + gy * g_dst.h / kHeight,
          (gx + gw) * g_dst.w / kWidth - gx * g_dst.w / kWidth,
          (gy + gh) * g_dst.h / kHeight - gy * g_dst.h / kHeight};
}

void DrawPointer() {
  bool overPanel = g_panel.open();
  bool visible = overPanel || (g_cursorVisible && g_cursorVisible());
  if (!visible) return;
  // A right-click is a help-click (Shift + click) for the game, which makes it
  // switch to its "HELP" pointer; keep the normal arrow for that - the HELP
  // pointer only shows when Shift is really held.
  bool helpFromRightClick = (NowUs() < g_helpShiftUntil || g_scriptShift) && !g_realShift;
  bool useArrow = overPanel || (g_cursor.shape == kHelpPointerShape && helpFromRightClick);
  const Cursor& c = (useArrow && g_arrow.w) ? g_arrow : g_cursor;
  if (!c.w) return;
  bool isArrow = &c == &g_arrow;
  if (!g_ptrTex || g_ptrVersion != c.version || g_ptrIsArrow != isArrow) {
    if (g_ptrTex) SDL_DestroyTexture(g_ptrTex);
    g_ptrTex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, c.w, c.h);
    if (!g_ptrTex) return;
    std::vector<uint32_t> px(size_t(c.w) * c.h);
    for (size_t i = 0; i < px.size(); ++i) px[i] = c.mask[i] ? g_pal32[c.pix[i]] : 0;
    SDL_UpdateTexture(g_ptrTex, nullptr, px.data(), c.w * 4);
    SDL_SetTextureBlendMode(g_ptrTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_ptrTex, g_settings.filter == Filter::kSharp ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    g_ptrVersion = c.version;
    g_ptrIsArrow = isArrow;
  }
  SDL_Rect r = ToOutput(g_mouse.x + c.offX, g_mouse.y + c.offY, c.w, c.h);
  SDL_RenderCopy(g_renderer, g_ptrTex, nullptr, &r);
}

void DrawFrame() {
  UpdateLayout();
  if (g_confineDirty) { g_confineDirty = false; UpdateMouseConfinement(); }
  // Rebuild the picture only when the game changed it (or the filter did).
  if (std::memcmp(g_lastFb, g_fb, sizeof g_fb) != 0 || std::memcmp(g_lastPal, g_pal32, sizeof g_pal32) != 0 ||
      g_builtFilter != g_settings.filter) {
    std::memcpy(g_lastFb, g_fb, sizeof g_fb);
    std::memcpy(g_lastPal, g_pal32, sizeof g_pal32);
    g_builtFilter = g_settings.filter;
    g_imageDirty = true;
    g_ptrVersion = -1;  // palette may have changed the pointer colours
  }
  if (g_imageDirty) UploadGameImage();
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  DrawGameImage();
  g_imageDirty = false;
  {
    Lock lock;
    if (g_uiVisible) {
      if (g_uiDirty) {
        g_panel.Draw(g_uiPixels, g_settings, OnMainMenu(), g_menuButtonHover);
        SDL_UpdateTexture(g_uiTex, nullptr, g_uiPixels, kWidth * 4);
        g_uiDirty = false;
      }
      SDL_RenderCopy(g_renderer, g_uiTex, nullptr, &g_dst);
    }
    DrawPointer();
  }
  if (!g_winshotPath.empty()) {
    std::vector<uint8_t> argb(size_t(g_outW) * g_outH * 4), rgba(argb.size());
    if (SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, argb.data(), g_outW * 4) == 0)
      WritePngRGBA(g_winshotPath, g_outW, g_outH, argb.data());
    g_winshotPath.clear();
  }
  SDL_RenderPresent(g_renderer);
}

DWORD WINAPI RenderThread(void*) {
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    Log("SDL_Init failed: %s", SDL_GetError());
    SetEvent(g_renderReady);
    return 1;
  }
  Uint32 flags = SDL_WINDOW_RESIZABLE | (g_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  g_window = SDL_CreateWindow("Ascendancy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              kWidth * 2, kHeight * 2, flags);
  if (g_window) {
    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (g_renderer) {
    g_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
    g_uiTex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
    if (g_uiTex) {
      SDL_SetTextureBlendMode(g_uiTex, SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(g_uiTex, SDL_ScaleModeLinear);
    }
  }
  if (!g_tex || !g_uiTex) {
    Log("could not create the game window: %s", SDL_GetError());
    SetEvent(g_renderReady);
    return 1;
  }
  SDL_ShowCursor(SDL_DISABLE);  // the pointer is drawn as an overlay
  SDL_RendererInfo info;
  SDL_GetRendererInfo(g_renderer, &info);
  bool vsync = info.flags & SDL_RENDERER_PRESENTVSYNC;
  Log("video: SDL %s renderer%s, %s, filter %s", info.name, vsync ? " (vsync)" : "",
      g_settings.fullscreen ? "borderless fullscreen" : "windowed", FilterKey(g_settings.filter));
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
  g_settings = cfg.settings;
  g_startUs = NowUs();
  for (int i = 0; i < 256; ++i) SetPalette(i, 0, 0, 0);
  if (g_bigFont.Parse(cfg.bigFont) && g_smallFont.Parse(cfg.smallFont))
    g_panel.SetFonts(&g_bigFont, &g_smallFont);
  else
    Log("options panel: game fonts not found - panel disabled");
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
void MarkDirty() {}

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
  Mouse m = g_mouse;
  return m;
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

uint8_t ShiftFlags() { return uint8_t(g_shift | g_scriptShift); }

void SetCursorImage(const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& mask, int w,
                    int h, int offX, int offY, int shape) {
  Lock lock;
  g_cursor.pix = pixels;
  g_cursor.mask = mask;
  g_cursor.w = w;
  g_cursor.h = h;
  g_cursor.offX = offX;
  g_cursor.offY = offY;
  g_cursor.shape = shape;
  ++g_cursor.version;
  if (shape == 0 && !g_arrow.w) { g_arrow = g_cursor; g_arrow.version = 1 << 30; }
}

void SetCursorVisibilityQuery(bool (*fn)()) { g_cursorVisible = fn; }

CameraInput TakeCameraInput() {
  if (g_cfg.headless) Present();
  Lock lock;
  CameraInput in = g_camInput;
  g_camInput = CameraInput{};
  return in;
}

void SetCameraArea(int x0, int y0, int x1, int y1, int screen) {
  Lock lock;
  g_camArea.x0 = x0; g_camArea.y0 = y0; g_camArea.x1 = x1; g_camArea.y1 = y1;
  g_camArea.screen = screen;
}
void SetScreenQuery(int (*fn)()) { g_screenQuery = fn; }

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
