// Manifest suggester (slice D2b). GPL-2.0. See suggest.hpp for scope.
#include "ask/suggest.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "loader/elf_parser.hpp"

namespace fs = std::filesystem;
namespace pnr::ask {
namespace {
constexpr size_t kMaxEntries = 5000;
constexpr int kMaxDepth = 4;

bool has_elf_magic(const fs::path& p) {
  std::ifstream f(p, std::ios::binary); char m[4] = {0};
  return f.read(m, 4) && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}
std::string sanitize_id(const std::string& base) {
  std::string o;
  for (unsigned char c : base) {
    if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
    else if (c == '.' || c == '-' || c == '_') o += static_cast<char>(c);
    else if (c == ' ') o += '-';
  }
  while (!o.empty() && (o[0] == '.' || o[0] == '-')) o.erase(0, 1);
  if (o.size() > 64) o.resize(64);
  return o;
}
std::string make_title(const std::string& base) {
  std::string t; bool up = true;
  for (unsigned char c : base) {
    if (c == '-' || c == '_' || c == ' ') { t += ' '; up = true; continue; }
    if (c < 0x20 || c == 0x7f || c == '#' || c == '=') continue;
    t += static_cast<char>(up ? std::toupper(c) : c); up = false;
  }
  return t;
}
}  // namespace

Suggestion suggest_manifest(const std::string& dir) {
  Suggestion s;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) { s.error = "not a directory: " + dir; return s; }
  const fs::path root = fs::absolute(dir, ec).lexically_normal();
  s.ok = true;
  size_t seen = 0; bool truncated = false; int skipped_other_arch = 0, skipped_so = 0;
  std::vector<std::pair<std::string, bool>> cands;   // relpath, is ET_EXEC
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it.depth() >= kMaxDepth) { it.disable_recursion_pending(); }
    if (++seen > kMaxEntries) { truncated = true; break; }
    std::error_code e2;
    if (it->is_symlink(e2)) { it.disable_recursion_pending(); continue; }   // symlinks are never followed
    if (!it->is_regular_file(e2)) continue;
    if (!has_elf_magic(it->path())) continue;
    const std::string rel = fs::relative(it->path(), root, e2).generic_string();
    auto pr = pnr::loader::parse_elf(it->path().string());
    if (!pr.ok) { s.notes.push_back("skipped (ELF header did not validate): " + rel); continue; }
    if (!pnr::loader::is_x86_64(pr.info)) { ++skipped_other_arch; continue; }
    if (rel.find(".so") != std::string::npos) { ++skipped_so; continue; }
    if (!pnr::loader::is_executable_type(pr.info)) { continue; }
    cands.push_back({rel, pr.info.elf_type == 2});
  }
  if (truncated) s.notes.push_back("only the first " + std::to_string(kMaxEntries) + " entries (depth " + std::to_string(kMaxDepth) + ") were looked at");
  if (skipped_other_arch) s.notes.push_back(std::to_string(skipped_other_arch) + " ELF file(s) that are not x86-64 were ignored");
  if (skipped_so) s.notes.push_back(std::to_string(skipped_so) + " shared library file(s) (.so) were ignored");
  std::sort(cands.begin(), cands.end());
  for (const auto& c : cands) s.candidates.push_back(c.first);

  std::string base = root.filename().string(); if (base.empty() || base == ".") base = root.parent_path().filename().string();
  std::string id = sanitize_id(base);
  s.fields.push_back({"id", id.empty() ? "my-game" : id, id.empty() ? "NOT INFERRED" : "guessed", "from the directory name; must be a unique directory-safe name"});
  std::string title = make_title(base);
  s.fields.push_back({"title", title.empty() ? "My Game" : title, title.empty() ? "NOT INFERRED" : "guessed", "from the directory name"});
  s.fields.push_back({"version", "0.1.0", "NOT INFERRED", "placeholder: nothing on disk states a version; set it yourself"});
  if (cands.empty()) {
    s.fields.push_back({"executable", "", "NOT INFERRED", "no native x86-64 Linux ELF executable was found"});
  } else {
    size_t pick = 0; std::string why = "the only x86-64 ELF executable found";
    if (cands.size() > 1) {
      why = "one of " + std::to_string(cands.size()) + " candidates; chosen by name and path depth, check it";
      int best = -1;
      for (size_t i = 0; i < cands.size(); ++i) {
        const std::string leaf = fs::path(cands[i].first).filename().string();
        int score = 0;
        if (sanitize_id(leaf) == id) score += 4;
        if (leaf == "game" || leaf == "main" || leaf == "run") score += 2;
        if (cands[i].first.find('/') == std::string::npos) score += 1;
        if (score > best) { best = score; pick = i; }
      }
    }
    s.fields.push_back({"executable", cands[pick].first, "guessed", why});
    s.complete = true;
  }
  s.fields.push_back({"arch", "x86-64", cands.empty() ? "guessed" : "read", cands.empty() ? "the only supported value" : "the chosen file's ELF header says x86-64"});
  return s;
}

std::string render_draft(const Suggestion& s) {
  std::string o = "# DRAFT manifest.pnr, suggested by PNR (Experimental, deterministic, no model).\n"
                  "# Nothing was installed or changed. Review every line before building a package.\n";
  for (const auto& f : s.fields) {
    o += "# " + f.key + ": " + f.how + (f.note.empty() ? "" : " - " + f.note) + "\n";
    o += f.key + " = " + f.value + "\n";
  }
  if (s.candidates.size() > 1) { o += "# other candidates:"; for (const auto& c : s.candidates) o += " " + c; o += "\n"; }
  for (const auto& n : s.notes) o += "# note: " + n + "\n";
  o += "# Not covered: checksums.sha256 and the .pnrgame archive itself (build them with PNR's packaging tools).\n";
  return o;
}

int cmd_manifest(const std::vector<std::string>& pos, const std::string& out_file, std::string* out) {
  if (pos.size() != 2 || pos[0] != "suggest") { *out += "usage: pnr-launcher manifest suggest <dir> [--out FILE]\n"; return 2; }
  Suggestion s = suggest_manifest(pos[1]);
  if (!s.ok) { *out += "manifest suggest: " + s.error + "\n"; return 2; }
  const std::string draft = render_draft(s);
  *out += draft;
  if (!out_file.empty()) {
    int fd = ::open(out_file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);   // never overwrites
    if (fd < 0) { *out += "NOT WRITTEN: cannot create " + out_file + " (it must not exist yet)\n"; return 2; }
    bool wrote = ::write(fd, draft.data(), draft.size()) == static_cast<ssize_t>(draft.size()); ::close(fd);
    if (!wrote) { *out += "NOT WRITTEN: write to " + out_file + " failed\n"; return 2; }
    *out += "written to " + out_file + " (a draft: guessed fields still need your review)\n";
  } else *out += "(nothing written; use --out FILE to save this draft)\n";
  if (!s.complete) { *out += "NO USABLE EXECUTABLE FOUND: this draft cannot be packaged as it is.\n"; return 1; }
  return 0;
}

}  // namespace pnr::ask
