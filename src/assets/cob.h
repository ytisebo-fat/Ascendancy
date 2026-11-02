#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace asc {

// COB archive (ASCEND00/01/02.COB).
//   u32  count
//   char names[count][50]   NUL-padded, e.g. "data\\game.pal"
//   u32  offsets[count]     absolute; a file ends where the next begins
//                           (the last one ends at end of archive)
class CobArchive {
 public:
  struct Entry {
    std::string name;  // as stored in the archive
    uint32_t offset = 0;
    uint32_t size = 0;
  };

  bool Open(const std::string& path, std::string* error = nullptr);

  const std::vector<Entry>& entries() const { return entries_; }
  const std::string& path() const { return path_; }

  // Name lookup is case-insensitive and accepts '/' or '\\'.
  const Entry* Find(const std::string& name) const;
  bool Read(const Entry& e, std::vector<uint8_t>* out) const;

  static std::string NormalizeName(const std::string& name);

 private:
  std::string path_;
  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t> index_;
};

// The set of archives listed in COB.CFG, searched in order - this mirrors how
// the original game resolves a path like "data\\game.pal".
class CobFileSystem {
 public:
  bool Mount(const std::string& path, std::string* error = nullptr);
  // Reads COB.CFG in gameDir and mounts every archive it lists.
  bool MountFromConfig(const std::string& gameDir, std::string* error = nullptr);
  bool ReadFile(const std::string& name, std::vector<uint8_t>* out) const;
  const std::vector<std::unique_ptr<CobArchive>>& archives() const { return archives_; }

 private:
  std::vector<std::unique_ptr<CobArchive>> archives_;
};

}  // namespace asc
