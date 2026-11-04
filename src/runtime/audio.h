#pragma once
// Sound: a replacement for the Miles Audio Interface Library's digital
// sample functions (AIL_*), mixed with SDL audio.
//
// The game's own sound code (ssystem.cpp) is left as it is: it still picks
// the effects (soundfx.txt, .voc files) and streams the music (music.txt,
// .raw files) through a double buffer exactly as it did for the Sound
// Blaster; only the library calls underneath are replaced.
//
// On top of that, the port can vary the button click (the game uses one
// sound for every button): see ClickSound in settings.h.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "runtime/settings.h"

namespace asc {
class GameImage;
}

namespace asc::audio {

struct Config {
  // Reads a file from the game's archives (e.g. "data\\click08.voc").
  std::function<bool(const char* name, std::vector<uint8_t>* out)> readGameFile;
};

// Opens the audio device and hooks the AIL functions. Without an audio
// device the game runs silently, as it did without a sound card.
bool Install(GameImage& image, const Config& cfg, const Settings& settings);

// Volumes and click sound; may be called from any thread.
void ApplySettings(const Settings& s);

// Plays one button click with the current settings (options panel preview).
void PreviewClick();

}  // namespace asc::audio
