// Live PSN library fetch (slice 2). GPL-2.0.
// Flow (from the MIT psn-api sources): NPSSO cookie -> authorize (302, code in
// Location) -> token exchange -> persisted-query GET for purchased games,
// paged; one refresh on 401. Access/refresh tokens live in memory for one
// call only. Untested against live Sony: tested only against fakes.
#pragma once
#include <string>
#include <vector>

#include "psn/http.hpp"
#include "psn/psn.hpp"

namespace pnr::psn {

struct SyncOptions {
  int page_size = 50;   // 1..100
  int max_pages = 200;
};
struct SyncReport {
  int pages = 0, requests = 0, refreshes = 0;
  ParseReport parse;    // accumulated over pages
  bool complete = false;  // paging reached a short or empty page
};

// Hosts PNR will talk to. Anything else is refused before sending.
std::vector<std::string> official_hosts();

// Fetches every page of purchased games. `http` is wrapped by the allow-list
// guard internally. The npsso value is sent only as a Cookie header to the
// authorize URL.
Status fetch_purchased_games(HttpClient& http, const std::string& npsso, const SyncOptions& opt,
                             std::vector<library::OwnedRecord>* rows, SyncReport* rep);

// Extracted for tests.
bool parse_code_from_location(const std::string& location, std::string* code);
std::string build_purchased_url(int start, int size);

}  // namespace pnr::psn
