// PNR Game Services - save data model (Core slice C5), GPL-2.0.
// Maps a title's "save directories" onto the jailed per-user, per-game save area of the VirtualFileSystem.
// Implemented, Tested. This is the HOST model only: PNR does not yet bind the PlayStation SaveData API
// (structure-heavy, layouts unverified). The jail is application-level path routing, not a security sandbox.
#pragma once
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "services/virtual_fs.hpp"

namespace pnr::services {

class SaveDataService {
 public:
  // quota_bytes limits the total size of one save directory (default 8 MiB).
  SaveDataService(const VirtualFileSystem& vfs, std::string user_id, std::string game_id, std::uint64_t quota_bytes = 8ull << 20);

  // Directory and file names: 1..32 chars of [A-Za-z0-9._-], not starting with '.', no separators.
  static bool valid_name(const std::string& n);

  Status create_dir(const std::string& dir);
  Status write(const std::string& dir, const std::string& file, const std::string& data);
  Status read(const std::string& dir, const std::string& file, std::string* out) const;
  Status remove(const std::string& dir, const std::string& file);
  Status remove_dir(const std::string& dir);
  std::vector<std::string> list_dirs() const;
  std::vector<std::string> list_files(const std::string& dir) const;
  std::uint64_t usage(const std::string& dir) const;
  const std::string& host_root() const { return root_; }

 private:
  std::string path(const std::string& dir) const;
  VirtualFileSystem vfs_;
  std::string user_, game_, root_;
  std::uint64_t quota_;
};

}  // namespace pnr::services
