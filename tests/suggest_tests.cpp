// Slice D2b tests: the manifest suggester (deterministic, read-only).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ask/suggest.hpp"

namespace fs = std::filesystem;
using namespace pnr::ask;
namespace {
bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }
std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
void spit(const std::string& p, const std::string& s) { fs::create_directories(fs::path(p).parent_path()); std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }
const Field* fld(const Suggestion& s, const std::string& k) { for (auto& f : s.fields) if (f.key == k) return &f; return nullptr; }
// Same parser rules as the real manifest.pnr reader: '#' comments, key = value.
std::map<std::string, std::string> parse_kv(const std::string& t) {
  std::map<std::string, std::string> m; size_t i = 0;
  while (i < t.size()) { size_t e = t.find('\n', i); if (e == std::string::npos) e = t.size(); std::string l = t.substr(i, e - i); i = e + 1;
    auto h = l.find('#'); if (h != std::string::npos) l.erase(h); auto eq = l.find('='); if (eq == std::string::npos) continue;
    auto tr = [](std::string s) { s.erase(0, s.find_first_not_of(" \t")); s.erase(s.find_last_not_of(" \t") + 1); return s; };
    m[tr(l.substr(0, eq))] = tr(l.substr(eq + 1)); }
  return m;
}
}  // namespace

void pnr_suggest_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  const std::string d = "/tmp/pnr_sug_" + std::to_string(::getpid());
  fs::remove_all(d); fs::create_directories(d);
  const std::string elf = slurp("/bin/true");
  const bool have_elf = elf.size() > 64 && elf.compare(0, 4, "\x7f" "ELF") == 0;
  if (!have_elf) { std::printf("SKIP suggester ELF tests: /bin/true is not an ELF here\n"); fs::remove_all(d); return; }

  { Suggestion s = suggest_manifest(d + "/nope"); check(!s.ok && has(s.error, "not a directory"), "suggest: a missing directory is an error"); }
  { Suggestion s = suggest_manifest(""); check(!s.ok, "suggest: an empty path is an error"); }

  // one ELF: found, guessed fields marked
  const std::string one = d + "/My Cool_Game";
  spit(one + "/bin/mycool", elf); ::chmod((one + "/bin/mycool").c_str(), 0755);
  spit(one + "/readme.txt", "hello");
  {
    Suggestion s = suggest_manifest(one);
    check(s.ok && s.complete && s.candidates.size() == 1 && s.candidates[0] == "bin/mycool", "suggest: the single x86-64 ELF is the candidate (relative path)");
    check(fld(s, "executable") && fld(s, "executable")->value == "bin/mycool" && fld(s, "executable")->how == "guessed", "suggest: the executable is marked guessed");
    check(fld(s, "id") && fld(s, "id")->value == "my-cool_game" && fld(s, "id")->how == "guessed", "suggest: id is a sanitised directory name, marked guessed");
    check(fld(s, "title") && fld(s, "title")->value == "My Cool Game", "suggest: title comes from the directory name");
    check(fld(s, "version") && fld(s, "version")->how == "NOT INFERRED", "suggest: version is never invented, it is NOT INFERRED");
    check(fld(s, "arch") && fld(s, "arch")->value == "x86-64" && fld(s, "arch")->how == "read", "suggest: arch is read from the ELF header");
    std::string draft = render_draft(s); auto kv = parse_kv(draft);
    check(kv["id"] == "my-cool_game" && kv["executable"] == "bin/mycool" && kv["arch"] == "x86-64" && kv["version"] == "0.1.0", "draft: parses with the manifest.pnr key = value rules");
    check(has(draft, "# DRAFT") && has(draft, "NOT INFERRED") && has(draft, "guessed"), "draft: says it is a draft and marks every guess");
    check(has(draft, "checksums.sha256"), "draft: says what it does not cover");
  }

  // nothing nonexistent is written; read-only
  {
    std::string before; for (auto& e : fs::recursive_directory_iterator(one)) before += e.path().string() + std::to_string(e.is_regular_file() ? fs::file_size(e.path()) : 0);
    std::string out; int rc = cmd_manifest({"suggest", one}, "", &out);
    std::string after; for (auto& e : fs::recursive_directory_iterator(one)) after += e.path().string() + std::to_string(e.is_regular_file() ? fs::file_size(e.path()) : 0);
    check(rc == 0 && before == after && has(out, "nothing written"), "cmd: without --out nothing is written and the directory is unchanged");
  }

  // --out writes once, never overwrites
  {
    const std::string of = d + "/draft.pnr"; std::string out;
    int rc = cmd_manifest({"suggest", one}, of, &out);
    check(rc == 0 && fs::exists(of) && has(slurp(of), "# DRAFT") && has(out, "written to"), "cmd: --out writes the draft");
    const std::string first = slurp(of); out.clear();
    rc = cmd_manifest({"suggest", one}, of, &out);
    check(rc == 2 && has(out, "NOT WRITTEN") && slurp(of) == first, "cmd: --out refuses to overwrite an existing file");
    if (::symlink("/tmp/pnr_sug_target", (d + "/link.pnr").c_str()) != 0) std::printf("note: symlink failed\n");
    out.clear();
    rc = cmd_manifest({"suggest", one}, d + "/link.pnr", &out);
    check(rc == 2 && !fs::exists("/tmp/pnr_sug_target"), "cmd: --out never follows a symlink");
  }

  // several candidates, other arch, .so, PE, symlink, empty
  const std::string many = d + "/coolgame";
  spit(many + "/coolgame", elf); spit(many + "/tools/helper", elf); spit(many + "/lib/libx.so.1", elf);
  { std::string arm = elf; arm[18] = static_cast<char>(0xB7); arm[19] = 0; spit(many + "/arm_tool", arm); }
  spit(many + "/win.exe", std::string("MZ") + std::string(100, 'x'));
  spit(many + "/SONYISH.pkg", std::string("\x7f" "PKG") + std::string(100, 'x'));
  if (::symlink("/bin/true", (many + "/linked").c_str()) != 0) std::printf("note: symlink failed\n");
  {
    Suggestion s = suggest_manifest(many);
    check(s.candidates.size() == 2 && s.candidates[0] == "coolgame" && s.candidates[1] == "tools/helper", "suggest: ignores .so files, non-x86-64 ELF, PE files, Sony-looking containers and symlinks");
    check(fld(s, "executable")->value == "coolgame" && has(fld(s, "executable")->note, "check it"), "suggest: with several candidates it picks by name and says to check");
    bool arm_note = false, so_note = false; for (auto& n : s.notes) { if (has(n, "not x86-64")) arm_note = true; if (has(n, ".so")) so_note = true; }
    check(arm_note && so_note, "suggest: it says what it ignored");
    check(has(render_draft(s), "other candidates: coolgame tools/helper"), "draft: lists the other candidates");
  }
  {
    const std::string none = d + "/empty"; spit(none + "/a.txt", "x"); spit(none + "/w.exe", "MZ");
    std::string out; int rc = cmd_manifest({"suggest", none}, "", &out);
    check(rc == 1 && has(out, "NO USABLE EXECUTABLE") && has(out, "executable = \n"), "cmd: nothing usable gives exit 1 and says so");
    check(cmd_manifest({"suggest"}, "", &out) == 2 && cmd_manifest({"other", none}, "", &out) == 2, "cmd: wrong usage is an error");
  }
  {  // depth cap and entry cap behave
    const std::string deep = d + "/deep"; spit(deep + "/a/b/c/d/e/f/game", elf); spit(deep + "/x.txt", "x");
    Suggestion s = suggest_manifest(deep);
    check(s.ok && s.candidates.empty(), "suggest: the search depth is capped");
  }
  fs::remove_all(d);
}
