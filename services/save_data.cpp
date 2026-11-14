// PNR Game Services - save data model (Core slice C5), GPL-2.0. See save_data.hpp.
#include "services/save_data.hpp"

#include <algorithm>
#include <filesystem>

namespace pnr::services {
namespace fs = std::filesystem;

SaveDataService::SaveDataService(const VirtualFileSystem& vfs, std::string user_id, std::string game_id, std::uint64_t quota)
    : vfs_(vfs), user_(std::move(user_id)), game_(std::move(game_id)), quota_(quota) {
  root_ = vfs_.saves(user_, game_).root();
}

bool SaveDataService::valid_name(const std::string& n) {
  if (n.empty() || n.size() > 32 || n[0] == '.') return false;
  return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

std::string SaveDataService::path(const std::string& dir) const { return root_ + "/" + dir; }

Status SaveDataService::create_dir(const std::string& dir) {
  if (!valid_name(dir)) return Status::Fail(2, "savedata", "invalid save directory name");
  if (!VirtualFileSystem::valid_id(user_) || !VirtualFileSystem::valid_id(game_)) return Status::Fail(2, "savedata", "invalid user or game id");
  return vfs_.saves(user_, game_).create_dirs("/" + dir);
}

std::uint64_t SaveDataService::usage(const std::string& dir) const {
  std::error_code ec;
  std::uint64_t total = 0;
  if (!valid_name(dir)) return 0;
  for (auto& e : fs::directory_iterator(path(dir), ec))
    if (e.is_regular_file(ec)) total += e.file_size(ec);
  return total;
}

Status SaveDataService::write(const std::string& dir, const std::string& file, const std::string& data) {
  if (!valid_name(dir) || !valid_name(file)) return Status::Fail(2, "savedata", "invalid directory or file name");
  std::error_code ec;
  if (!fs::is_directory(path(dir), ec)) return Status::Fail(3, "savedata", "save directory does not exist");
  std::uint64_t old = 0;
  if (fs::is_regular_file(path(dir) + "/" + file, ec)) old = fs::file_size(path(dir) + "/" + file, ec);
  const std::uint64_t now = usage(dir) - old + data.size();
  if (now > quota_) return Status::Fail(4, "savedata", "save directory quota exceeded; nothing was written");
  return vfs_.saves(user_, game_).write_file("/" + dir + "/" + file, data);
}

Status SaveDataService::read(const std::string& dir, const std::string& file, std::string* out) const {
  if (!out) return Status::Fail(2, "savedata", "output required");
  if (!valid_name(dir) || !valid_name(file)) return Status::Fail(2, "savedata", "invalid directory or file name");
  if (!vfs_.saves(user_, game_).exists("/" + dir + "/" + file)) return Status::Fail(3, "savedata", "no such save file");
  return vfs_.saves(user_, game_).read_file("/" + dir + "/" + file, *out);
}

Status SaveDataService::remove(const std::string& dir, const std::string& file) {
  if (!valid_name(dir) || !valid_name(file)) return Status::Fail(2, "savedata", "invalid directory or file name");
  std::error_code ec;
  if (!fs::remove(path(dir) + "/" + file, ec) || ec) return Status::Fail(3, "savedata", "no such save file");
  return Status::Ok();
}

Status SaveDataService::remove_dir(const std::string& dir) {
  if (!valid_name(dir)) return Status::Fail(2, "savedata", "invalid save directory name");
  std::error_code ec;
  if (!fs::is_directory(path(dir), ec)) return Status::Fail(3, "savedata", "no such save directory");
  fs::remove_all(path(dir), ec);
  return ec ? Status::Fail(5, "savedata", "could not remove: " + ec.message()) : Status::Ok();
}

std::vector<std::string> SaveDataService::list_dirs() const {
  std::vector<std::string> o;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(root_, ec)) if (e.is_directory(ec)) o.push_back(e.path().filename().string());
  std::sort(o.begin(), o.end());
  return o;
}

std::vector<std::string> SaveDataService::list_files(const std::string& dir) const {
  std::vector<std::string> o;
  std::error_code ec;
  if (!valid_name(dir)) return o;
  for (auto& e : fs::directory_iterator(path(dir), ec)) if (e.is_regular_file(ec)) o.push_back(e.path().filename().string());
  std::sort(o.begin(), o.end());
  return o;
}

}  // namespace pnr::services
