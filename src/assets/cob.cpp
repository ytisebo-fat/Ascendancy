#include "assets/cob.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace asc {
namespace {

constexpr size_t kNameSize = 50;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void SetError(std::string* e, const std::string& msg) {
  if (e) *e = msg;
}

}  // namespace

std::string CobArchive::NormalizeName(const std::string& name) {
  std::string s;
  s.reserve(name.size());
  for (char c : name) s.push_back(c == '/' ? '\\' : char(std::tolower((unsigned char)c)));
  size_t start = 0;
  while (start < s.size() && (s[start] == '\\' || s[start] == '.')) {
    // strip leading "\" and ".\" so "\data\x" and ".\data\x" match "data\x"
    if (s[start] == '.' && (start + 1 >= s.size() || s[start + 1] != '\\')) break;
    ++start;
  }
  return s.substr(start);
}

bool CobArchive::Open(const std::string& path, std::string* error) {
  path_ = path;
  entries_.clear();
  index_.clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return SetError(error, "cannot open " + path), false;
  std::fseek(f, 0, SEEK_END);
  long fileSize = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);

  uint8_t hdr[4];
  if (std::fread(hdr, 1, 4, f) != 4) {
    std::fclose(f);
    return SetError(error, "truncated header in " + path), false;
  }
  uint32_t count = Le32(hdr);
  if (count == 0 || count > 100000 || 4 + count * (kNameSize + 4) > uint64_t(fileSize)) {
    std::fclose(f);
    return SetError(error, "not a COB archive: " + path), false;
  }
  std::vector<uint8_t> dir(count * (kNameSize + 4));
  if (std::fread(dir.data(), 1, dir.size(), f) != dir.size()) {
    std::fclose(f);
    return SetError(error, "truncated directory in " + path), false;
  }
  std::fclose(f);

  entries_.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const char* n = reinterpret_cast<const char*>(&dir[i * kNameSize]);
    entries_[i].name.assign(n, strnlen(n, kNameSize));
    entries_[i].offset = Le32(&dir[count * kNameSize + i * 4]);
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t end = (i + 1 < count) ? entries_[i + 1].offset : uint32_t(fileSize);
    if (end < entries_[i].offset || end > uint32_t(fileSize))
      return SetError(error, "bad offsets in " + path), false;
    entries_[i].size = end - entries_[i].offset;
    index_.emplace(NormalizeName(entries_[i].name), i);
  }
  return true;
}

const CobArchive::Entry* CobArchive::Find(const std::string& name) const {
  auto it = index_.find(NormalizeName(name));
  return it == index_.end() ? nullptr : &entries_[it->second];
}

bool CobArchive::Read(const Entry& e, std::vector<uint8_t>* out) const {
  FILE* f = std::fopen(path_.c_str(), "rb");
  if (!f) return false;
  out->resize(e.size);
  bool ok = std::fseek(f, long(e.offset), SEEK_SET) == 0 &&
            std::fread(out->data(), 1, e.size, f) == e.size;
  std::fclose(f);
  return ok;
}

bool CobFileSystem::Mount(const std::string& path, std::string* error) {
  auto a = std::make_unique<CobArchive>();
  if (!a->Open(path, error)) return false;
  archives_.push_back(std::move(a));
  return true;
}

bool CobFileSystem::MountFromConfig(const std::string& gameDir, std::string* error) {
  std::string dir = gameDir;
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
  FILE* f = std::fopen((dir + "cob.cfg").c_str(), "rb");
  if (!f) f = std::fopen((dir + "COB.CFG").c_str(), "rb");
  if (!f)
    return SetError(error, "cannot open COB.CFG in \"" + gameDir +
                               "\" - give the Ascendancy game folder (the one with ASCEND00.COB)"),
           false;
  char line[256];
  bool any = false;
  while (std::fgets(line, sizeof line, f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    if (s.empty()) continue;
    // Archives may be stored in any case on disk; try as-is, lower, upper.
    std::string lower = s, upper = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    std::string err;
    if (Mount(dir + s, &err) || Mount(dir + lower, &err) || Mount(dir + upper, &err)) {
      any = true;
    } else {
      std::fclose(f);
      return SetError(error, err), false;
    }
  }
  std::fclose(f);
  if (!any) SetError(error, "COB.CFG lists no archives");
  return any;
}

bool CobFileSystem::ReadFile(const std::string& name, std::vector<uint8_t>* out) const {
  for (const auto& a : archives_)
    if (const auto* e = a->Find(name)) return a->Read(*e, out);
  return false;
}

}  // namespace asc
