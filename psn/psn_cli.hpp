// Command logic for 'pnr-launcher psn ...' (offline slice). Each function
// returns the exit code and writes the text it wants shown to *out. Tokens
// are never written to *out. GPL-2.0.
#pragma once
#include <string>

#include "library/game_db.hpp"

namespace pnr::psn {

// PNR_PSN_TOKEN_FILE if set, else <parent of db>/psn/npsso.token.
std::string token_path_for(const std::string& db_path);

int cmd_status(library::GameDb& db, const std::string& token_path, bool json, std::string* out);
// `input` is the pasted text (read by the caller from stdin or a file).
int cmd_set_token(const std::string& token_path, const std::string& input, bool json, std::string* out);
int cmd_import_json(library::GameDb& db, const std::string& file, bool complete, bool json, std::string* out);
int cmd_owned(library::GameDb& db, bool json, const std::string& platform_filter, std::string* out);
int cmd_link(library::GameDb& db, const std::string& entitlement_id, const std::string& game_id, bool json, std::string* out);
int cmd_unlink(library::GameDb& db, const std::string& entitlement_id, bool json, std::string* out);
int cmd_disconnect(library::GameDb& db, const std::string& token_path, bool forget_owned, bool yes, bool json, std::string* out);

}  // namespace pnr::psn
