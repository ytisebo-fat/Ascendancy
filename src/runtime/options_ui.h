#pragma once
// The port's own options panel, drawn on top of the game (640x480 logical
// coordinates) in the game's visual style, using the game's own fonts.
// Pure drawing + hit-testing; the render thread owns it and routes input.
#include <cstdint>
#include <string>

#include "assets/font.h"
#include "runtime/settings.h"

namespace asc::ui {

struct Rect {
  int x, y, w, h;
  bool Contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum class Action {
  kNone,
  kClose,
  kChanged,  // settings changed: apply + save
};

class OptionsPanel {
 public:
  void SetFonts(const FntFont* big, const FntFont* small) { big_ = big; small_ = small; }
  bool fontsOk() const { return big_ && small_; }

  bool open() const { return open_; }
  void Open() { open_ = true; hover_ = -1; }
  void Close() { open_ = false; }

  // Main-menu button (top-right corner).
  static Rect MenuButton() { return {546, 8, 86, 22}; }

  // Returns true if the hover state changed (needs redraw).
  bool OnMove(int x, int y);
  Action OnClick(int x, int y, Settings& s);

  // Draws into a 640x480 ARGB buffer (cleared to transparent first).
  void Draw(uint32_t* argb, const Settings& s, bool menuButton, bool menuButtonHover) const;

 private:
  enum Id {
    kFullscreen, kWindow, kFilterPrev, kFilterNext, kHelpOn, kHelpOff, kAspect43,
    kMusicDown, kMusicUp, kFxDown, kFxUp, kClickPrev, kClickNext, kClose, kCount
  };
  static Rect ItemRect(int id);

  const FntFont* big_ = nullptr;
  const FntFont* small_ = nullptr;
  bool open_ = false;
  int hover_ = -1;
};

}  // namespace asc::ui
