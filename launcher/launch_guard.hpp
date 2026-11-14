// Pre-launch checks shared by the CLI and the desktop UI, so both start a game
// the same way (GPL-2.0). Extracted from pnr-launcher in slice UI-1.
#pragma once
#include <string>

#include "launcher/launcher_core.hpp"

namespace pnr::launcher {

// Does what pnr-launcher does before core.launch():
//  - re-verifies games installed from a pinned-key store source (B1b);
//  - with `sandbox`, requires a usable sandbox (no silent fallback) and
//    installs the child-setup hook that enters it. PARTIAL isolation, see docs/sandbox.md.
// Returns "" when the launch may go ahead, otherwise the full refusal text
// ("LAUNCH NOT STARTED: ..."). An unknown game id is left for launch() to report.
std::string prepare_launch(LauncherCore& core, const std::string& db_path, const std::string& game_id, bool sandbox);

}  // namespace pnr::launcher
