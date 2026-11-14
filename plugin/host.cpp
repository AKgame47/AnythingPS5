// PNR plugin system - live broker host (slice P4), GPL-2.0. See host.hpp.
#include "plugin/host.hpp"

#include <filesystem>

#include "library/game_db.hpp"
#include "store/download.hpp"
#include "store/store.hpp"

namespace pnr::plugin {
namespace {
std::string jq(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += (char)c; } else if (c < 0x20) o += ' '; else o += (char)c; }
  return o + "\"";
}
}  // namespace

LiveBrokerHost::LiveBrokerHost(std::string db_path, std::string plugin_id) : db_(std::move(db_path)), plugin_(std::move(plugin_id)) {}

bool LiveBrokerHost::library_list(std::string* json, std::string* err) {
  std::error_code ec;
  if (!std::filesystem::exists(db_, ec)) { *json = "[]"; return true; }  // never create a database on a plugin's behalf
  library::GameDb db;
  auto st = db.open(db_);
  if (!st.ok) { *err = "library could not be opened"; return false; }
  *json = "[";
  bool first = true;
  for (auto& g : db.list_games()) { *json += std::string(first ? "" : ",") + "{\"id\":" + jq(g.game_id) + ",\"title\":" + jq(g.title) + "}"; first = false; }
  *json += "]";
  db.close();
  return true;
}

bool LiveBrokerHost::store_add_source(const std::string& id, const std::string& url, const std::string& rights, std::string* err) {
  if (rights.empty()) { *err = "a rights statement is required"; return false; }
  store::Registry reg(store::store_dir_for(db_));
  auto ls = reg.load();
  if (!ls.ok) { *err = "store registry could not be read"; return false; }
  auto s = reg.add(id, url, "declared by plugin " + plugin_ + " (not verified by PNR): " + rights);
  if (!s.ok) { *err = s.error.message; return false; }
  return true;
}

bool LiveBrokerHost::download_request(const std::string& source, const std::string& pkg, std::string* err) {
  store::Registry reg(store::store_dir_for(db_));
  auto ls = reg.load();
  if (!ls.ok) { *err = "store registry could not be read"; return false; }
  store::Queue q(reg, store::store_dir_for(db_));
  store::DlState st;
  auto s = q.enqueue(source, pkg, &st);
  if (!s.ok) { *err = s.error.message; return false; }
  return true;
}

}  // namespace pnr::plugin
