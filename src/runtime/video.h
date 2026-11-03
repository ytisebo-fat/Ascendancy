#pragma once
// Presentation of the game's 640x480 8-bit screen in a modern window (SDL2).
//
// The window lives on its own render thread: it shows the game's framebuffer
// at the monitor refresh rate (vsync) independent of the game's own frame
// rate, processes window/input events, applies the picture filter, and draws
// the port's own layers on top: the options panel (F12 / main-menu button)
// and the mouse pointer at the live mouse position. The game thread only
// writes the framebuffer and palette.
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/settings.h"

namespace asc::video {

constexpr int kWidth = 640, kHeight = 480;

struct Config {
  Settings settings;              // initial settings (from ascendancy.ini + command line)
  std::string settingsPath;       // where the options panel saves changes ("" = don't save)
  std::vector<uint8_t> bigFont, smallFont;  // game fonts (FNT) for the options panel
  bool headless = false;          // no window (testing); frames only via screenshots
  int screenshotEveryMs = 0;      // testing: dump frames as PNG at this interval
  std::string screenshotDir = ".";
};

bool Init(const Config& cfg);
void Shutdown();

// The emulated video memory: 640*480 bytes, row-major.
uint8_t* Framebuffer();
void MarkDirty();

// VGA DAC palette, 6 bits per channel.
void SetPalette(int index, uint8_t r, uint8_t g, uint8_t b);
void GetPalette(int index, uint8_t* r, uint8_t* g, uint8_t* b);

// Called by the game thread at idle points (input polls, refreshes): handles
// headless scripted input and periodic screenshots. Cheap.
void Present(bool force = false);
// Paces like a 60 Hz vertical retrace.
void WaitVblank();

bool SaveScreenshot(const std::string& path);

// Input state from the window, in game coordinates (0..639, 0..479).
struct Mouse { int x = 320, y = 240; int buttons = 0; };  // bit0 left, bit1 right
Mouse GetMouse();
Mouse GetMouseNoPump();
void WarpMouse(int x, int y);
// Called after input events were processed (render thread, or game thread
// when headless).
void SetInputListener(void (*fn)());

// Keyboard, BIOS style: (scan code << 8) | ASCII.
bool PeekKey(uint16_t* key);
bool PopKey(uint16_t* key);
uint8_t ShiftFlags();   // bit0 right shift, 1 left shift, 2 ctrl, 3 alt

// Mouse pointer overlay: image (palette indices + opacity mask), the offset
// of its top-left corner from the hotspot, and the game's shape number.
void SetCursorImage(const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& mask, int w,
                    int h, int offX, int offY, int shape);
void SetCursorVisibilityQuery(bool (*visibleFn)());
// Which game screen is showing (0 = main menu); used for the Options button.
void SetScreenQuery(int (*screenFn)());

// Camera input for 3D views (wheel zoom, middle-button drag), collected on
// the render thread and taken once per game frame by the game thread.
struct CameraInput {
  int zoomSteps = 0;        // + = further away, - = closer
  float yawDegrees = 0, tiltDegrees = 0;
  bool recenter = false;    // middle click without dragging
};
CameraInput TakeCameraInput();
// Screen area (game coordinates, inclusive) and game screen where the camera
// controls apply.
void SetCameraArea(int x0, int y0, int x1, int y1, int screen);

// Testing aid: scripted input, e.g. "3000:click:320:293,6000:key:esc".
void SetInputScript(const std::string& script);

}  // namespace asc::video
