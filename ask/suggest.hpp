// Manifest suggester (slice D2b). GPL-2.0. Status: Experimental.
//
// Read-only helper that looks at a directory and prints a DRAFT manifest.pnr.
// It is deterministic: no model is involved. It never installs, imports,
// executes or modifies anything, and writes a file only when the user passes
// --out (and never over an existing file). Every field it did not read from a
// file is marked "guessed" or "NOT INFERRED"; the user must review the draft.
// It only recognises native x86-64 Linux ELF executables. It does not read
// Sony containers or any proprietary format (those files are simply not
// candidates).
#pragma once
#include <string>
#include <vector>

namespace pnr::ask {

struct Field { std::string key, value, how, note; };   // how: "read" | "guessed" | "NOT INFERRED"

struct Suggestion {
  bool ok = false;                       // a directory was scanned
  std::string error;
  std::vector<Field> fields;             // id, title, version, executable, arch
  std::vector<std::string> candidates;   // x86-64 ELF executables found, relative paths
  std::vector<std::string> notes;        // non-fatal observations (skipped files, truncation, ...)
  bool complete = false;                 // executable was found; id/title/version are still guesses
};

Suggestion suggest_manifest(const std::string& dir);
// Draft text: "# ..." comment lines plus key = value lines that parse as manifest.pnr.
std::string render_draft(const Suggestion& s);
// CLI: `manifest suggest <dir> [--out FILE]`. Returns 0 draft printed, 1 nothing usable found, 2 error.
int cmd_manifest(const std::vector<std::string>& pos, const std::string& out_file, std::string* out);

}  // namespace pnr::ask
