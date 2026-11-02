#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace asc {

// Writes an RGBA8 image as PNG (uncompressed deflate; no zlib dependency).
// Used by the tools for asset inspection and by the runtime for screenshots.
bool WritePngRGBA(const std::string& path, int width, int height,
                  const uint8_t* rgba);

}  // namespace asc
