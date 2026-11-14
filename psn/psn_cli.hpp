// Command logic for 'pnr-launcher psn ...' (slice 1 offline commands plus slice 2 live sync). Each function
// returns the exit code and writes the text it wants shown to *out. Tokens
// are never written to *out. GPL-2.0.
#pragma once
#include <functional>
#include <string>

#include "library/game_db.hpp"
#include "psn/http.hpp"

namespace pnr::psn {

// PNR_PSN_TOKEN_FILE if set, else <parent of db>/psn/npsso.token.
std::string token_path_for(const std::string& db_path);

int cmd_status(library::GameDb& db, const std::string& token_path, bool json, std::string* out);
// `input` is the pasted text (read by the caller from stdin or a file).
int cmd_set_token(const std::string& token_path, const std::string& input, bool json, std::string* out);
int cmd_import_json(library::GameDb& db, const std::string& file, bool complete, bool json, std::string* out);
// Consent for any command that talks to Sony live (F1 warning gate).
// `flag` = the caller passed --live-ok (bypasses the prompt, never the warning
// text). Otherwise an interactive caller supplies `confirm`, which shows the
// warning and returns true only on an explicit yes. With neither, the command
// refuses and names the flag. Offline commands never need this.
struct LiveConsent {
  bool flag = false;
  bool interactive = false;
  std::function<bool(const std::string& warning)> confirm;  // used only when interactive
};
// The exact warning shown before a live request.
const char* live_warning();

// Live sync. `client` is for tests (a fake); null uses libcurl with TLS
// verification. Default is merge; `complete` marks absent rows as not returned.
int cmd_sync(library::GameDb& db, const std::string& token_path, bool complete, bool json, const LiveConsent& consent, std::string* out, HttpClient* client = nullptr);
int cmd_owned(library::GameDb& db, bool json, const std::string& platform_filter, std::string* out);
int cmd_link(library::GameDb& db, const std::string& entitlement_id, const std::string& game_id, bool json, std::string* out);
int cmd_unlink(library::GameDb& db, const std::string& entitlement_id, bool json, std::string* out);
int cmd_disconnect(library::GameDb& db, const std::string& token_path, bool forget_owned, bool yes, bool json, std::string* out);

}  // namespace pnr::psn
