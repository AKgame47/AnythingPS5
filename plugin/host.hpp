// PNR plugin system - live broker host (slice P4), GPL-2.0.
// Connects the permission broker to the real library database and store registry. The plugin never gets a handle: each call is
// checked against the grants, validated again here, and goes through the same code paths the user's own commands use.
#pragma once
#include <string>

#include "plugin/runner.hpp"

namespace pnr::plugin {

class LiveBrokerHost : public BrokerHost {
 public:
  // db_path is the library database (PNR_LIBRARY_DB). plugin_id labels what a plugin declares.
  LiveBrokerHost(std::string db_path, std::string plugin_id);
  bool library_list(std::string* json, std::string* err) override;
  bool store_add_source(const std::string& id, const std::string& url, const std::string& rights, std::string* err) override;
  bool download_request(const std::string& source, const std::string& pkg, std::string* err) override;
 private:
  std::string db_, plugin_;
};

}  // namespace pnr::plugin
