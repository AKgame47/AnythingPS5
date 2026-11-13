// PNR package scan pipeline interface (slice B3). GPL-2.0.
//
// Stages: 1) hash (always on), 2) model score (EMBER2024 LightGBM through an
// external Python helper; "off" unless BOTH a model file and a helper are
// configured), 3) ClamAV second opinion ("off" unless clamscan exists).
// A stage that is not running says "off" with the reason; nothing is faked.
//
// HONEST SCOPE: detection is high but not absolute. A clean result is NOT a
// guarantee a package is safe. EMBER2024 models score PE, ELF, APK and PDF
// files, so the model stage only looks at ELF files inside a .pnrgame, never at
// the package container. The model stage is Experimental: it was NOT run against
// real thrember or a real model in the development sandbox (no lightgbm there).
#pragma once
#include <string>
#include <vector>

namespace pnr::scan {

struct StageResult {
  std::string stage;   // "hash" | "model" | "clamav"
  std::string state;   // "ok" | "flagged" | "off" | "error" | "not-applicable"
  std::string detail;
};

struct Config {
  std::string interpreter = "python3";  // runs the helper
  std::string helper;                   // path to the scan helper script; empty = model stage off
  std::string model;                    // path to an EMBER2024 ELF .model file; empty = model stage off
  double threshold = 0.5;               // score >= threshold is "flagged". Not calibrated by PNR.
  std::string clamscan = "clamscan";    // name or path
  int timeout_seconds = 120;
};
// Reads PNR_SCAN_PYTHON, PNR_SCAN_HELPER, PNR_SCAN_MODEL, PNR_SCAN_THRESHOLD, PNR_SCAN_CLAMSCAN.
Config config_from_env();

struct Report {
  std::vector<StageResult> stages;
  bool flagged() const;
  std::string text() const;   // multi-line, phone-readable, includes the honesty note
};

Report scan_package(const std::string& pnrgame_path, const std::string& expect_sha256, long long expect_size, const Config& cfg);

}  // namespace pnr::scan
