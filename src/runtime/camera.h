#pragma once
// Mouse-wheel zoom and middle-button rotate/tilt for the 3D galaxy view
// (star map, COSMOSWnd), driving the game's own camera functions.
namespace asc {

class GameImage;

bool InstallCameraControls(GameImage& image);

}  // namespace asc
