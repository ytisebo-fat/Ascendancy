// ascx - Ascendancy asset tool.
//
//   ascx list    <archive.cob>
//   ascx extract <archive.cob> <outdir>
//   ascx shp2png <file.shp|file.tmp> <palette.pal> <out_prefix>
//   ascx dumpall <gamedir> <outdir>     every SHP/TMP in every archive -> PNG
//                                        (using DATA\GAME.PAL)
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "assets/cob.h"
#include "assets/palette.h"
#include "assets/shp.h"
#include "common/png_write.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

namespace {

bool ReadAll(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  out->resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

void MakeDirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/' || path[i] == '\\') MKDIR(path.substr(0, i).c_str());
}

std::string ToHostPath(std::string name) {
  for (char& c : name) {
    if (c == '\\') c = '/';
    c = char(std::tolower((unsigned char)c));
  }
  while (!name.empty() && name[0] == '/') name.erase(0, 1);
  return name;
}

int ShpToPng(const std::vector<uint8_t>& shpData, const asc::Palette& pal, const std::string& prefix) {
  asc::ShpFile shp;
  if (!shp.Parse(shpData)) {
    std::fprintf(stderr, "  %s: not a valid SHP\n", prefix.c_str());
    return 0;
  }
  int written = 0;
  for (size_t i = 0; i < shp.size(); ++i) {
    const auto& f = shp.frame(i);
    if (f.empty()) continue;
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "_%03zu.png", i);
    auto rgba = asc::ShpFile::ToRGBA(f, pal);
    if (asc::WritePngRGBA(prefix + suffix, f.width, f.height, rgba.data())) ++written;
  }
  return written;
}

}  // namespace

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  std::string err;
  if (cmd == "list" && argc == 3) {
    asc::CobArchive a;
    if (!a.Open(argv[2], &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    for (const auto& e : a.entries()) std::printf("%10u  %s\n", e.size, e.name.c_str());
    std::printf("%zu files\n", a.entries().size());
    return 0;
  }
  if (cmd == "extract" && argc == 4) {
    asc::CobArchive a;
    if (!a.Open(argv[2], &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    std::vector<uint8_t> buf;
    for (const auto& e : a.entries()) {
      std::string out = std::string(argv[3]) + "/" + ToHostPath(e.name);
      MakeDirs(out.substr(0, out.find_last_of('/')));
      if (!a.Read(e, &buf)) return std::fprintf(stderr, "read failed: %s\n", e.name.c_str()), 1;
      FILE* f = std::fopen(out.c_str(), "wb");
      if (!f) return std::fprintf(stderr, "cannot write %s\n", out.c_str()), 1;
      std::fwrite(buf.data(), 1, buf.size(), f);
      std::fclose(f);
    }
    std::printf("extracted %zu files\n", a.entries().size());
    return 0;
  }
  if (cmd == "shp2png" && argc == 5) {
    std::vector<uint8_t> shp, pal;
    if (!ReadAll(argv[2], &shp) || !ReadAll(argv[3], &pal)) return std::fprintf(stderr, "read failed\n"), 1;
    int n = ShpToPng(shp, asc::Palette::FromVga(pal), argv[4]);
    std::printf("wrote %d frames\n", n);
    return 0;
  }
  if (cmd == "dumpall" && argc == 4) {
    asc::CobFileSystem fs;
    if (!fs.MountFromConfig(argv[2], &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    std::vector<uint8_t> palData;
    if (!fs.ReadFile("data\\game.pal", &palData)) return std::fprintf(stderr, "no game.pal\n"), 1;
    asc::Palette pal = asc::Palette::FromVga(palData);
    int files = 0, frames = 0;
    std::vector<uint8_t> buf;
    for (const auto& a : fs.archives()) {
      for (const auto& e : a->entries()) {
        std::string lower = ToHostPath(e.name);
        if (lower.size() < 4) continue;
        std::string ext = lower.substr(lower.size() - 4);
        if (ext != ".shp" && ext != ".tmp") continue;
        if (!a->Read(e, &buf)) continue;
        std::string prefix = std::string(argv[3]) + "/" + lower;
        MakeDirs(prefix.substr(0, prefix.find_last_of('/')));
        frames += ShpToPng(buf, pal, prefix);
        ++files;
      }
    }
    std::printf("%d sprite files, %d frames written\n", files, frames);
    return 0;
  }
  std::fprintf(stderr,
               "usage:\n"
               "  ascx list    <archive.cob>\n"
               "  ascx extract <archive.cob> <outdir>\n"
               "  ascx shp2png <file.shp> <palette.pal> <out_prefix>\n"
               "  ascx dumpall <gamedir> <outdir>\n");
  return 2;
}
