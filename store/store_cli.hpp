// Command logic for 'pnr-launcher store ...'. Returns the exit code and
// writes text to *out. GPL-2.0.
#pragma once
#include <string>

#include "store/download.hpp"
#include "store/store.hpp"

namespace pnr::store {

int cmd_source_add(Registry& r, const std::string& id, const std::string& url, const std::string& rights, bool json, std::string* out);
int cmd_source_remove(Registry& r, const std::string& id, bool yes, bool json, std::string* out);
int cmd_source_pin(Registry& r, const std::string& id, const std::string& pubkey_hex, bool json, std::string* out);  // "none" unpins
int cmd_source_list(Registry& r, bool json, std::string* out);
// `client` null = libcurl with TLS verification (tests inject a fake).
int cmd_refresh(Registry& r, const std::string& id_or_empty, bool json, std::string* out, psn::HttpClient* client = nullptr);
// text empty = list all. Searches id, name, description, category (case-insensitive).
int cmd_search(Registry& r, const std::string& text, const std::string& source, bool json, std::string* out);
int cmd_show(Registry& r, const std::string& source, const std::string& pkg, bool json, std::string* out);

// Download queue. `get` only queues; `queue run` transfers (resuming any .part).
int cmd_get(Queue& q, const std::string& source, const std::string& pkg, bool json, std::string* out);
int cmd_queue_list(Queue& q, bool json, std::string* out);
// pkg empty = every queued/partial item. `client` null = libcurl; tests inject a fake and options.
int cmd_queue_run(Queue& q, const std::string& source, const std::string& pkg, bool json, std::string* out, psn::HttpClient* client = nullptr, const DownloadOptions* opt = nullptr);
int cmd_queue_pause(Queue& q, const std::string& source, const std::string& pkg, bool pause, bool json, std::string* out);  // pause=false resumes
int cmd_queue_cancel(Queue& q, const std::string& source, const std::string& pkg, bool yes, bool json, std::string* out);

}  // namespace pnr::store
