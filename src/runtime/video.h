#pragma once
// Presentation of the game's 640x480 8-bit screen in a modern window (SDL2).
//
// The window lives on its own render thread: it shows the game's framebuffer
// at the monitor refresh rate (vsync) independent of the game's own frame
// rate, processes window/input events, and draws the mouse pointer as an
// overlay at the current mouse position (no lag, no flicker). The game
// thread only writes the framebuffer and palette.
#include <cstdint>
#include <string>
#include <vector>

namespace asc::video {

constexpr int kWidth = 640, kHeight = 480;

struct Config {
  bool fullscreen = true;         // borderless fullscreen (desktop resolution)
  bool smooth = false;            // linear filtering instead of sharp pixels
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
// Called (on the render thread) after input events were processed.
void SetInputListener(void (*fn)());

// Keyboard, BIOS style: (scan code << 8) | ASCII.
bool PeekKey(uint16_t* key);
bool PopKey(uint16_t* key);
uint8_t ShiftFlags();   // bit0 right shift, 1 left shift, 2 ctrl, 3 alt

// Mouse pointer overlay: image (palette indices + opacity mask) and the
// offset of its top-left corner from the hotspot. `visibleFn` is polled by
// the render thread each frame.
void SetCursorImage(const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& mask, int w,
                    int h, int offX, int offY);
void SetCursorVisibilityQuery(bool (*visibleFn)());

// Testing aid: scripted input, e.g. "3000:click:320:293,6000:key:esc".
void SetInputScript(const std::string& script);

}  // namespace asc::video
