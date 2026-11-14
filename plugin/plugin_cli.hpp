// Command logic for 'pnr-launcher plugin ...' (slice P1). Returns the exit code, text goes to *out. GPL-2.0.
#pragma once
#include <string>
#include <vector>

#include "plugin/manager.hpp"

namespace pnr::plugin {
// args: everything after "plugin": add <dir> | list | info <id> | enable <id> | disable <id> | remove <id> [--yes handled by caller].
// grant: comma separated permission names from --grant. json: machine-readable output for list and info.
int cmd_plugin(PluginManager& pm, const std::vector<std::string>& args, const std::string& grant, bool yes, bool json, std::string* out);
}  // namespace pnr::plugin
