#include "services/virtual_fs.hpp"

#include <stdexcept>

namespace pnr::services {

namespace {
const char* area_dir(FsArea a) {
  switch (a) {
    case FsArea::installed_content: return "installed_content";
    case FsArea::user_config: return "user_config";
    case FsArea::save_data: return "save_data";
    case FsArea::shader_cache: return "shader_cache";
    case FsArea::runtime_cache: return "runtime_cache";
    case FsArea::compat_metadata: return "compat_metadata";
    case FsArea::logs: return "logs";
  }
  return "unknown";
}
}  // namespace

VirtualFileSystem::VirtualFileSystem(std::string root) : root_(std::move(root)) {}

bool VirtualFileSystem::valid_id(const std::string& id) {
  if (id.empty() || id[0] == '.') return false;
  return id.find('/') == std::string::npos && id.find("..") == std::string::npos &&
         id.find('\\') == std::string::npos;
}

compat::FileSystemService VirtualFileSystem::area(FsArea a) const {
  return compat::FileSystemService(root_ + "/" + area_dir(a));
}

compat::FileSystemService VirtualFileSystem::saves(const std::string& user_id,
                                           const std::string& game_id) const {
  if (!valid_id(user_id) || !valid_id(game_id))
    throw std::invalid_argument("invalid user or game id for save area");
  return compat::FileSystemService(root_ + "/save_data/" + user_id + "/" + game_id);
}

}  // namespace pnr::services
