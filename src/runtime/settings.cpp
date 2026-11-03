#include "runtime/settings.h"

#include <cstdio>
#include <cstring>

namespace asc {

const char* FilterName(Filter f) {
  switch (f) {
    case Filter::kSharp: return "Sharp pixels";
    case Filter::kCrisp: return "Crisp";
    case Filter::kSoft: return "Soft";
    case Filter::kSmooth: return "Smooth art";
    case Filter::kCrt: return "CRT monitor";
    default: return "?";
  }
}

const char* FilterDescription(Filter f) {
  switch (f) {
    case Filter::kSharp: return "Plain enlarged pixels, like the original.";
    case Filter::kCrisp: return "Sharp, even pixels with smooth edges. Recommended.";
    case Filter::kSoft: return "Soft and blurry, hides the pixels.";
    case Filter::kSmooth: return "Rounds off jagged pixel edges (Scale2x).";
    case Filter::kCrt: return "Scanlines like a 1995 monitor.";
    default: return "";
  }
}

const char* FilterKey(Filter f) {
  switch (f) {
    case Filter::kSharp: return "sharp";
    case Filter::kCrisp: return "crisp";
    case Filter::kSoft: return "soft";
    case Filter::kSmooth: return "smooth";
    case Filter::kCrt: return "crt";
    default: return "crisp";
  }
}

bool Settings::Load(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    char key[64], val[64];
    if (std::sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
    if (!std::strcmp(key, "fullscreen")) fullscreen = std::strcmp(val, "0") != 0;
    else if (!std::strcmp(key, "right_click_help")) rightClickHelp = std::strcmp(val, "0") != 0;
    else if (!std::strcmp(key, "filter")) {
      for (int i = 0; i < int(Filter::kCount); ++i)
        if (!std::strcmp(val, FilterKey(Filter(i)))) filter = Filter(i);
    }
  }
  std::fclose(f);
  return true;
}

bool Settings::Save(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fprintf(f,
               "; Ascendancy native port settings (also changeable in-game: F12)\n"
               "[display]\n"
               "fullscreen = %d\n"
               "filter = %s        ; sharp, crisp, soft, smooth, crt\n"
               "\n[mouse]\n"
               "right_click_help = %d\n",
               fullscreen ? 1 : 0, FilterKey(filter), rightClickHelp ? 1 : 0);
  std::fclose(f);
  return true;
}

}  // namespace asc
