// PNR Game Services - MODULE 08: Virtual Filesystem (v1)
// Separate jailed areas for installed content, user config, saves (isolated
// per game and user), shader cache, runtime cache, compatibility metadata
// and logs. Built on the Module 03 jailed FileSystemService: traversal
// outside an area root is rejected. Status: Implemented, Tested.
#pragma once
#include <string>
#include "compat/services.hpp"
#include "core/runtime.hpp"

namespace pnr::services {

enum class FsArea {
  installed_content,   // game payload files (read-only for games)
  user_config,         // per-user settings
  save_data,           // per-game, per-user saves
  shader_cache,        // compiled shader cache
  runtime_cache,       // misc runtime cache
  compat_metadata,     // per-game compatibility data
  logs                 // diagnostic logs
};

class VirtualFileSystem {
public:
  // root: base directory; a sub-directory per area is used beneath it.
  explicit VirtualFileSystem(std::string root);

  // Per-game, per-user save area: <root>/save_data/<user>/<game>/...
  compat::FileSystemService saves(const std::string& user_id,
                          const std::string& game_id) const;
  compat::FileSystemService area(FsArea a) const;

  // Validates an id for use as a path component: non-empty, no '/', no
  // "..", no leading '.'. Returns false for anything else.
  static bool valid_id(const std::string& id);

private:
  std::string root_;
};

}  // namespace pnr::services
