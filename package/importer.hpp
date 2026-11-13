// .pnrgame package format v1 + controlled importer (MODULE: installer).
// A .pnrgame is a ustar archive:
//   manifest.pnr       key=value: id, title, version, executable, arch, min_runtime
//   checksums.sha256   "<hex>  <path>" for every other file
//   <payload files>    executable + assets
// Protected/proprietary console packages are NOT this format: they fail
// cleanly at "not a .pnrgame" instead of being misread. Importing never
// bypasses licensing: this format carries only developer-controlled
// content with optional (currently unused) signature slot.
// Status: Implemented, Tested.
#pragma once
#include <string>

#include "core/runtime.hpp"
#include "library/game_db.hpp"

namespace pnr::pkg {

struct Manifest {
  std::string id;
  std::string title;
  std::string version;
  std::string executable;  // path inside the package
  std::string arch = "x86-64";
  int64_t min_runtime = 1;
};

struct ImportResult {
  Status status;
  Manifest manifest;
  std::string install_path;  // where payload was extracted
};

// Validates structure, manifest fields, and every SHA-256 checksum;
// validates the executable as a native x86-64 ELF; extracts payload to
// <library_root>/games/<id>-<version>/ with traversal protection; then
// registers game + installation + compatibility(Untested) in the db.
ImportResult import_package(const std::string& package_path,
                            const std::string& library_root,
                            pnr::library::GameDb& db);

// Builds a .pnrgame from a manifest + payload files (dev tooling).
Status build_package(const std::string& out_path, const Manifest& m,
                     const std::vector<std::pair<std::string, std::string>>&
                         files);  // (package path, host path)

}  // namespace pnr::pkg
