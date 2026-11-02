#pragma once

namespace asc {

class GameImage;

// Replaces the game's VESA video driver (VESA480.DLL, Miles VFX) with a
// native one that renders through runtime/video.
bool InstallVfxDriver(GameImage& image, bool trace = false);

}  // namespace asc
