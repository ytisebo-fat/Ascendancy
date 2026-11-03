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

struct Settings {
  bool fullscreen = true;
  Filter filter = Filter::kCrisp;
  bool rightClickHelp = true;

  bool Load(const std::string& path);
  bool Save(const std::string& path) const;
};

}  // namespace asc
