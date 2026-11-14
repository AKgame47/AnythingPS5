#include "launcher/launch_guard.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "sandbox/sandbox.hpp"
#include "store/download.hpp"
#include "store/store.hpp"

namespace pnr::launcher {

std::string prepare_launch(LauncherCore& core, const std::string& db_path, const std::string& game_id, bool sandbox) {
  {  // slice B1b: games installed from a pinned-key source are re-verified before every launch
    pnr::store::Registry preg(pnr::store::store_dir_for(db_path));
    std::string note;
    if (preg.load()) {
      auto pv = pnr::store::check_provenance(preg.dir(), preg, game_id, &note);
      if (!pv) return "LAUNCH NOT STARTED: " + pv.error.message;
    }
  }
  if (!sandbox) return "";
  auto sp = pnr::sandbox::probe();  // no silent fallback: an unusable sandbox means the game is not started
  if (!sp.usable()) return "LAUNCH NOT STARTED: --sandbox was requested but the sandbox is not usable here (" + sp.notes + ")";
  GameEntry sge;
  if (core.get(game_id, &sge)) {  // an unknown id is reported by launch() itself, before anything is forked
    std::error_code aec;
    auto gd = std::filesystem::absolute(sge.install_path, aec).lexically_normal();
    std::string gds = gd.string();
    while (gds.size() > 1 && gds.back() == '/') gds.pop_back();
    if (aec || gds.empty()) return "LAUNCH NOT STARTED: cannot resolve the game directory for the sandbox";
    // the game's jail root and the PS-ABI shim dir are what launch() gives it; both are env in the child
    core.set_child_setup([gds](std::string* e) {
      pnr::sandbox::Policy pol;
      pol.game_dir = gds;
      if (const char* j = std::getenv("PNR_JAIL_ROOT"))
        if (*j) pol.rw_dirs.push_back(std::filesystem::absolute(j).lexically_normal().string());
      if (const char* l = std::getenv("LD_LIBRARY_PATH")) {
        std::string s = l;
        size_t i = 0;
        while (i <= s.size()) {
          size_t k = s.find(':', i);
          if (k == std::string::npos) k = s.size();
          if (k > i) pol.ro_dirs.push_back(std::filesystem::absolute(s.substr(i, k - i)).lexically_normal().string());
          i = k + 1;
        }
      }
      return pnr::sandbox::enter(pol, e);
    });
  }
  return "";
}

}  // namespace pnr::launcher
