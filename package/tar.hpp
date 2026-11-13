// Minimal ustar archive reader/writer for the .pnrgame package format.
// Regular files only, names <= 100 bytes (documented format limit).
// No compression in v1: integrity comes from SHA-256 checksums, not secrecy.
// Status: Implemented, Tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/runtime.hpp"

namespace pnr::pkg {

struct TarEntry {
  std::string name;          // relative path, '/'-separated
  std::vector<uint8_t> data;
};

// Writes entries to a ustar archive. Fails on names > 100 bytes.
Status tar_write(const std::string& out_path, const std::vector<TarEntry>& entries);

// Reads all regular-file entries. Fails on malformed headers or bad
// checksums. Directory entries are skipped.
Status tar_read(const std::string& in_path, std::vector<TarEntry>& out);

}  // namespace pnr::pkg
