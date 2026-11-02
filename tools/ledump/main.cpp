// ledump - inspect a DOS/4GW LE executable and export it for analysis.
//
//   ledump ascend.exe                     print header, objects, fixup stats
//   ledump ascend.exe --elf ascend.elf    write an ELF32 file with both objects
//                                         relocated at their preferred bases,
//                                         loadable by Ghidra / objdump
//   ledump ascend.exe --fixups out.txt    list every fixup (source -> target)
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "le/le_image.h"

using asc::LeImage;

namespace {

template <typename T>
void Append(std::vector<uint8_t>& v, const T& x) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
  v.insert(v.end(), p, p + sizeof(T));
}

#pragma pack(push, 1)
struct Elf32Ehdr {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version, entry, phoff, shoff, flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Elf32Phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Elf32Shdr { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; };
struct Elf32Sym { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; };
#pragma pack(pop)

// Trims trailing zero bytes from a data object so the rest becomes .bss.
size_t InitializedSize(const std::vector<uint8_t>& d) {
  size_t n = d.size();
  while (n > 0 && d[n - 1] == 0) --n;
  return (n + 15) & ~size_t(15);
}

bool WriteElf(const LeImage& le, const std::string& path) {
  auto objs = le.RelocatedAtPreferredBase();
  const auto& meta = le.objects();

  std::string shstr = std::string("\0", 1);
  auto addName = [&](const char* s) { uint32_t o = uint32_t(shstr.size()); shstr += s; shstr += '\0'; return o; };

  std::vector<uint8_t> out(sizeof(Elf32Ehdr), 0);
  uint32_t phoff = uint32_t(out.size());
  out.resize(out.size() + sizeof(Elf32Phdr) * meta.size());

  std::vector<Elf32Phdr> ph;
  std::vector<Elf32Shdr> sh(1);  // null section
  std::vector<uint32_t> fileOffs;
  for (size_t i = 0; i < meta.size(); ++i) {
    while (out.size() % 16) out.push_back(0);
    uint32_t off = uint32_t(out.size());
    size_t fsz = meta[i].executable() ? objs[i].size() : InitializedSize(objs[i]);
    if (fsz > objs[i].size()) fsz = objs[i].size();
    out.insert(out.end(), objs[i].begin(), objs[i].begin() + fsz);
    bool x = meta[i].executable();
    ph.push_back({1, off, meta[i].preferredBase, meta[i].preferredBase, uint32_t(fsz),
                  meta[i].virtualSize, uint32_t(x ? 5 : 6), 0x1000});
    Elf32Shdr s{};
    s.name = addName(x ? ".text" : ".data");
    s.type = 1;  // PROGBITS
    s.flags = x ? 0x6 : 0x3;
    s.addr = meta[i].preferredBase;
    s.offset = off;
    s.size = uint32_t(fsz);
    s.addralign = 16;
    sh.push_back(s);
    if (!x && fsz < meta[i].virtualSize) {
      Elf32Shdr b{};
      b.name = addName(".bss");
      b.type = 8;  // NOBITS
      b.flags = 0x3;
      b.addr = meta[i].preferredBase + uint32_t(fsz);
      b.offset = off + uint32_t(fsz);
      b.size = meta[i].virtualSize - uint32_t(fsz);
      b.addralign = 16;
      sh.push_back(b);
    }
  }
  std::memcpy(&out[phoff], ph.data(), ph.size() * sizeof(Elf32Phdr));

  // Symbol table: just the entry point for now.
  std::string strtab = std::string("\0", 1) + "_cstart_";
  strtab += '\0';
  std::vector<Elf32Sym> syms(2, Elf32Sym{});
  syms[1].name = 1;
  syms[1].value = meta[le.entryObject()].preferredBase + le.entryOffset();
  syms[1].info = 0x12;  // GLOBAL FUNC
  syms[1].shndx = 1;
  while (out.size() % 4) out.push_back(0);
  uint32_t symOff = uint32_t(out.size());
  for (auto& s : syms) Append(out, s);
  uint32_t strOff = uint32_t(out.size());
  out.insert(out.end(), strtab.begin(), strtab.end());

  Elf32Shdr symsh{};
  symsh.name = addName(".symtab");
  symsh.type = 2;
  symsh.offset = symOff;
  symsh.size = uint32_t(syms.size() * sizeof(Elf32Sym));
  symsh.link = uint32_t(sh.size() + 1);
  symsh.info = 1;
  symsh.addralign = 4;
  symsh.entsize = sizeof(Elf32Sym);
  sh.push_back(symsh);
  Elf32Shdr strsh{};
  strsh.name = addName(".strtab");
  strsh.type = 3;
  strsh.offset = strOff;
  strsh.size = uint32_t(strtab.size());
  strsh.addralign = 1;
  sh.push_back(strsh);
  Elf32Shdr shs{};
  shs.name = addName(".shstrtab");
  shs.type = 3;
  shs.addralign = 1;
  shs.offset = uint32_t(out.size());
  shs.size = uint32_t(shstr.size());
  sh.push_back(shs);
  out.insert(out.end(), shstr.begin(), shstr.end());
  while (out.size() % 4) out.push_back(0);
  uint32_t shoff = uint32_t(out.size());
  for (auto& s : sh) Append(out, s);

  Elf32Ehdr eh{};
  const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 1, 1, 1, 0};
  std::memcpy(eh.ident, ident, 16);
  eh.type = 2;      // EXEC
  eh.machine = 3;   // i386
  eh.version = 1;
  eh.entry = meta[le.entryObject()].preferredBase + le.entryOffset();
  eh.phoff = phoff;
  eh.shoff = shoff;
  eh.ehsize = sizeof(Elf32Ehdr);
  eh.phentsize = sizeof(Elf32Phdr);
  eh.phnum = uint16_t(ph.size());
  eh.shentsize = sizeof(Elf32Shdr);
  eh.shnum = uint16_t(sh.size());
  eh.shstrndx = uint16_t(sh.size() - 1);
  std::memcpy(out.data(), &eh, sizeof eh);

  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: ledump <game.exe> [--elf out.elf] [--fixups out.txt]\n");
    return 2;
  }
  LeImage le;
  std::string err;
  if (!le.Load(argv[1], &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  const auto& objs = le.objects();
  std::printf("entry: object %u + 0x%x  (linear 0x%x)\n", le.entryObject() + 1, le.entryOffset(),
              objs[le.entryObject()].preferredBase + le.entryOffset());
  std::printf("stack: object %u + 0x%x\n", le.stackObject() + 1, le.stackOffset());
  for (size_t i = 0; i < objs.size(); ++i)
    std::printf("object %zu: base 0x%08x size 0x%06x flags 0x%04x %s pages %u..%u\n", i + 1,
                objs[i].preferredBase, objs[i].virtualSize, objs[i].flags,
                objs[i].executable() ? "CODE" : "DATA", objs[i].pageIndex,
                objs[i].pageIndex + objs[i].pageCount - 1);
  std::map<std::pair<int, int>, int> stats;  // (type, src obj -> dst obj)
  std::map<int, int> byType;
  for (const auto& f : le.fixups()) byType[int(f.type)]++;
  std::printf("fixups: %zu total\n", le.fixups().size());
  for (auto& [t, n] : byType) std::printf("  type 0x%02x: %d\n", t, n);

  for (int i = 2; i + 1 < argc; i += 2) {
    std::string opt = argv[i], path = argv[i + 1];
    if (opt == "--elf") {
      if (!WriteElf(le, path)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
      std::printf("wrote %s\n", path.c_str());
    } else if (opt == "--fixups") {
      FILE* f = std::fopen(path.c_str(), "w");
      if (!f) return 1;
      for (const auto& x : le.fixups())
        std::fprintf(f, "%08x %02x -> %08x\n", objs[x.srcObject].preferredBase + x.srcOffset,
                     int(x.type), objs[x.dstObject].preferredBase + x.dstOffset);
      std::fclose(f);
      std::printf("wrote %s\n", path.c_str());
    }
  }
  return 0;
}
