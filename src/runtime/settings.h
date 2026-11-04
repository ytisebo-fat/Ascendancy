#pragma once
// Port settings, stored in ascendancy.ini next to the game.
#include <string>

namespace asc {

enum class Filter {
  kPixelPerfect,  // largest whole-number enlargement: every pixel exactly square (wider black border)
  kCrisp,    // integer pre-scale + smooth final step: sharp but even pixels
  kSoft,     // bilinear: soft, slightly blurry
  kSmooth,   // Scale2x pixel-art upscaler, then soft: rounded edges
  kCrt,      // crisp + scanlines
  kCount
};

const char* FilterName(Filter f);         // shown in the options panel
const char* FilterDescription(Filter f);  // one line explaining it
const char* FilterKey(Filter f);          // value in the ini file

// The button click (the game uses one sound for every button).
enum class ClickSound {
  kSoft,      // the game's click, quieter and mellower (default)
  kOriginal,  // the game's click, identical every time
  kVaried,    // the game's click with subtle random pitch/volume changes
  kMixed,     // rotates through the game's (unused) click sounds, loudness-matched
  kCount
};
const char* ClickSoundName(ClickSound c);
const char* ClickSoundDescription(ClickSound c);
const char* ClickSoundKey(ClickSound c);

struct Settings {
  bool fullscreen = true;
  Filter filter = Filter::kCrisp;
  bool rightClickHelp = true;
  int musicVolume = 7;    // 0..10
  int effectsVolume = 8;  // 0..10
  ClickSound clickSound = ClickSound::kSoft;

  bool Load(const std::string& path);
  bool Save(const std::string& path) const;
};

}  // namespace asc
