#include "runtime/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace asc {

const char* FilterName(Filter f) {
  switch (f) {
    case Filter::kPixelPerfect: return "Pixel perfect";
    case Filter::kCrisp: return "Crisp";
    case Filter::kSoft: return "Soft";
    case Filter::kSmooth: return "Smooth art";
    case Filter::kCrt: return "CRT monitor";
    default: return "?";
  }
}

const char* FilterDescription(Filter f) {
  switch (f) {
    case Filter::kPixelPerfect: return "Exact square pixels, whole-number size, black border.";
    case Filter::kCrisp: return "Sharp, even pixels with smooth edges. Recommended.";
    case Filter::kSoft: return "Soft and blurry, hides the pixels.";
    case Filter::kSmooth: return "Rounds off jagged pixel edges (Scale2x).";
    case Filter::kCrt: return "Scanlines like a 1995 monitor.";
    default: return "";
  }
}

const char* FilterKey(Filter f) {
  switch (f) {
    case Filter::kPixelPerfect: return "pixel";
    case Filter::kCrisp: return "crisp";
    case Filter::kSoft: return "soft";
    case Filter::kSmooth: return "smooth";
    case Filter::kCrt: return "crt";
    default: return "crisp";
  }
}

const char* ClickSoundName(ClickSound c) {
  switch (c) {
    case ClickSound::kSoft: return "Soft";
    case ClickSound::kOriginal: return "Original";
    case ClickSound::kVaried: return "Varied";
    case ClickSound::kMixed: return "Mixed";
    default: return "?";
  }
}

const char* ClickSoundDescription(ClickSound c) {
  switch (c) {
    case ClickSound::kSoft: return "The game's click, much quieter and mellower.";
    case ClickSound::kOriginal: return "The game's button click, the same every time.";
    case ClickSound::kVaried: return "The game's click, slightly different every time.";
    case ClickSound::kMixed: return "Takes turns with the game's other click sounds.";
    default: return "";
  }
}

const char* ClickSoundKey(ClickSound c) {
  switch (c) {
    case ClickSound::kSoft: return "soft";
    case ClickSound::kOriginal: return "original";
    case ClickSound::kVaried: return "varied";
    case ClickSound::kMixed: return "mixed";
    default: return "soft";
  }
}

namespace {
int Clamp10(int v) { return v < 0 ? 0 : v > 10 ? 10 : v; }
}  // namespace

bool Settings::Load(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    char key[64], val[64];
    if (std::sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
    if (!std::strcmp(key, "fullscreen")) fullscreen = std::strcmp(val, "0") != 0;
    else if (!std::strcmp(key, "right_click_help")) rightClickHelp = std::strcmp(val, "0") != 0;
    else if (!std::strcmp(key, "music_volume")) musicVolume = Clamp10(std::atoi(val));
    else if (!std::strcmp(key, "effects_volume")) effectsVolume = Clamp10(std::atoi(val));
    else if (!std::strcmp(key, "click")) {
      for (int i = 0; i < int(ClickSound::kCount); ++i)
        if (!std::strcmp(val, ClickSoundKey(ClickSound(i)))) clickSound = ClickSound(i);
    }
    else if (!std::strcmp(key, "filter")) {
      for (int i = 0; i < int(Filter::kCount); ++i)
        if (!std::strcmp(val, FilterKey(Filter(i)))) filter = Filter(i);
      if (!std::strcmp(val, "sharp")) filter = Filter::kPixelPerfect;  // older builds
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
               "filter = %s        ; pixel, crisp, soft, smooth, crt\n"
               "\n[mouse]\n"
               "right_click_help = %d\n"
               "\n[sound]\n"
               "music_volume = %d     ; 0..10\n"
               "effects_volume = %d   ; 0..10\n"
               "click = %s  ; soft, original, varied, mixed\n",
               fullscreen ? 1 : 0, FilterKey(filter), rightClickHelp ? 1 : 0, musicVolume,
               effectsVolume, ClickSoundKey(clickSound));
  std::fclose(f);
  return true;
}

}  // namespace asc
