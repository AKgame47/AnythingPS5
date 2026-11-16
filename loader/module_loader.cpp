// Core slice C1: see module_loader.hpp for scope and limits. GPL-2.0.
#include "loader/module_loader.hpp"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "loader/nid.hpp"
#include "loader/sony_symbols.hpp"

namespace pnr::loader {
extern "C" __attribute__((noreturn, optimize("no-stack-protector"))) void pnr_guest_exit() {
  long r = 231;  // exit_group(0): no TLS needed, safe with the guest FS base
  __asm__ volatile("syscall" : "+a"(r) : "D"(0) : "rcx", "r11", "memory");
  for (;;) {}
}

namespace {

using Bytes = std::vector<std::uint8_t>;
using Fn = std::function<std::int64_t(const std::vector<std::int64_t>&)>;

constexpr int kMaxSlots = 64;
struct SlotTable {
  std::mutex mu;
  Fn fn[kMaxSlots];
  bool used[kMaxSlots] = {};
};
SlotTable& slots() { static SlotTable t; return t; }

template <int I>
__attribute__((optimize("no-stack-protector"))) std::int64_t thunk(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d, std::int64_t e, std::int64_t f) {
  // Guest code runs with its own FS base (TLS/TCB). Host code needs the host FS, so switch for the call.
  const std::uint64_t g = pnr::loader::guest_to_host();
  const std::int64_t r = slots().fn[I](std::vector<std::int64_t>{a, b, c, d, e, f});
  pnr::loader::host_to_guest(g);
  return r;
}
using ThunkPtr = std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
template <int... I>
std::array<ThunkPtr, kMaxSlots> make_thunks(std::integer_sequence<int, I...>) { return {{&thunk<I>...}}; }
const std::array<ThunkPtr, kMaxSlots>& thunks() {
  static const auto t = make_thunks(std::make_integer_sequence<int, kMaxSlots>{});
  return t;
}

int claim_slot(const Fn& fn) {
  auto& t = slots();
  std::lock_guard<std::mutex> l(t.mu);
  for (int i = 0; i < kMaxSlots; ++i)
    if (!t.used[i]) { t.used[i] = true; t.fn[i] = fn; return i; }
  return -1;
}
void release_slot(int i) {
  auto& t = slots();
  std::lock_guard<std::mutex> l(t.mu);
  if (i >= 0 && i < kMaxSlots) { t.used[i] = false; t.fn[i] = nullptr; }
}

std::uint16_t u16(const Bytes& b, std::size_t o) { return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8)); }
std::uint32_t u32(const Bytes& b, std::size_t o) { return static_cast<std::uint32_t>(u16(b, o)) | (static_cast<std::uint32_t>(u16(b, o + 2)) << 16); }
std::uint64_t u64(const Bytes& b, std::size_t o) { return static_cast<std::uint64_t>(u32(b, o)) | (static_cast<std::uint64_t>(u32(b, o + 4)) << 32); }
bool in(const Bytes& b, std::uint64_t off, std::uint64_t len) { return off <= b.size() && len <= b.size() - off; }

struct Seg { std::uint32_t type, flags; std::uint64_t off, vaddr, filesz, memsz; };
struct Dyn {
  std::vector<Seg> loads;
  std::uint64_t entry = 0;
  std::uint64_t dyn_off = 0, dyn_size = 0;
  bool has_dyn = false;
  std::uint64_t symtab = 0, strtab = 0, strsz = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
  std::uint64_t init = 0, fini = 0, init_array = 0, init_arraysz = 0, fini_array = 0, fini_arraysz = 0;
  std::uint64_t soname = 0;  // DT_SONAME string offset
  std::vector<std::string> needed_offsets_unused;
  std::vector<std::uint64_t> needed;  // DT_NEEDED string offsets
  bool has_tls = false;
  std::uint64_t tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 1;
};

// File offset of a virtual address range inside a PT_LOAD's file-backed bytes, or -1.
std::int64_t v2o(const Dyn& d, std::uint64_t va, std::uint64_t len) {
  for (const auto& s : d.loads)
    if (va >= s.vaddr && len <= s.filesz && va - s.vaddr <= s.filesz - len) return static_cast<std::int64_t>(s.off + (va - s.vaddr));
  return -1;
}

pnr::Status parse_dyn(const Bytes& b, Dyn* d) {
  auto fail = [](int c, const std::string& m) { return pnr::Status::Fail(c, "module", m); };
  if (is_self_container(b))
    return fail(20, "this is a SELF container (signed or encrypted executable). PNR does not decrypt or unprotect files; give it a plain ELF you already have the right to use");
  if (!in(b, 0, 64) || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return fail(21, "not an ELF file");
  if (b[4] != 2 || b[5] != 1) return fail(22, "not a little-endian 64-bit ELF");
  if (u16(b, 0x12) != 62) return fail(23, "not an x86-64 ELF");
  if (u16(b, 0x10) != 3) return fail(24, "only position-independent (ET_DYN) modules are supported");
  d->entry = u64(b, 0x18);
  const std::uint64_t phoff = u64(b, 0x20);
  const std::uint16_t phentsize = u16(b, 0x36), phnum = u16(b, 0x38);
  if (phentsize < 56 || phnum == 0 || phnum > 64 || !in(b, phoff, static_cast<std::uint64_t>(phentsize) * phnum))
    return fail(25, "program headers out of bounds");
  for (unsigned i = 0; i < phnum; ++i) {
    const std::size_t o = phoff + static_cast<std::size_t>(i) * phentsize;
    Seg s{u32(b, o), u32(b, o + 4), u64(b, o + 8), u64(b, o + 16), u64(b, o + 32), u64(b, o + 40)};
    if (s.type == 1) {
      if (!in(b, s.off, s.filesz)) return fail(26, "PT_LOAD file range out of bounds");
      if (s.filesz > s.memsz) return fail(27, "PT_LOAD filesz larger than memsz");
      d->loads.push_back(s);
    } else if (s.type == 7) {
      if (s.filesz > s.memsz || s.memsz > (1u << 24)) return fail(30, "PT_TLS sizes are unreasonable");
      d->has_tls = true; d->tls_vaddr = s.vaddr; d->tls_filesz = s.filesz; d->tls_memsz = s.memsz;
      d->tls_align = u64(b, o + 48);
    } else if (s.type == 2) {
      if (!in(b, s.off, s.filesz)) return fail(28, "PT_DYNAMIC out of bounds");
      d->has_dyn = true; d->dyn_off = s.off; d->dyn_size = s.filesz;
    }
  }
  if (d->loads.empty()) return fail(29, "no PT_LOAD segments");
  if (!d->has_dyn) return pnr::Status::Ok();
  for (std::uint64_t o = d->dyn_off; o + 16 <= d->dyn_off + d->dyn_size; o += 16) {
    const std::int64_t tag = static_cast<std::int64_t>(u64(b, o));
    const std::uint64_t val = u64(b, o + 8);
    if (tag == 0) break;
    switch (tag) {
      case 1: d->needed.push_back(val); break;
      case 5: d->strtab = val; break;
      case 6: d->symtab = val; break;
      case 7: d->rela = val; break;
      case 8: d->relasz = val; break;
      case 10: d->strsz = val; break;
      case 23: d->jmprel = val; break;
      case 2: d->pltrelsz = val; break;
      case 12: d->init = val; break;
      case 13: d->fini = val; break;
      case 14: d->soname = val; break;
      case 25: d->init_array = val; break;
      case 27: d->init_arraysz = val; break;
      case 26: d->fini_array = val; break;
      case 28: d->fini_arraysz = val; break;
      case 11: if (val != 24) return fail(30, "unsupported DT_SYMENT"); break;
      case 9: if (val != 24) return fail(31, "unsupported DT_RELAENT"); break;
      default: break;
    }
  }
  return pnr::Status::Ok();
}

bool sym_name(const Bytes& b, const Dyn& d, std::uint32_t name_off, std::string* out) {
  if (name_off >= d.strsz) return false;
  const std::int64_t so = v2o(d, d.strtab, d.strsz);
  if (so < 0) return false;
  std::string s;
  for (std::uint64_t i = name_off; i < d.strsz; ++i) {
    const char c = static_cast<char>(b[so + i]);
    if (!c) { *out = s; return true; }
    s.push_back(c);
  }
  return false;  // unterminated
}

std::size_t sym_count(const Dyn& d) {
  if (d.symtab == 0 || d.strtab <= d.symtab) return 0;  // symbol table is expected right before the string table
  return static_cast<std::size_t>((d.strtab - d.symtab) / 24);
}

}  // namespace

bool is_self_container(const Bytes& b) {
  // SELF files start with the magic bytes 4F 15 3D 1D.
  return b.size() >= 4 && b[0] == 0x4F && b[1] == 0x15 && b[2] == 0x3D && b[3] == 0x1D;
}

pnr::Status resolve_imports_from_program_headers(const Bytes& b, const pnr::prx::PrxRegistry& reg, ImportResolution* out) {
  *out = ImportResolution{};
  if (in(b, 0, 0x14) && u16(b, 0x10) != 3) return pnr::Status::Ok();  // only ET_DYN modules are walked here
  Dyn d;
  auto st = parse_dyn(b, &d);
  if (!st) return st;
  if (!d.has_dyn || d.symtab == 0 || d.strtab == 0) return pnr::Status::Ok();
  const std::int64_t so = v2o(d, d.strtab, d.strsz);
  if (so < 0) return pnr::Status::Fail(32, "module", "string table out of bounds");
  for (auto n : d.needed) { std::string s; if (sym_name(b, d, static_cast<std::uint32_t>(n), &s)) out->needed_libs.push_back(s); }
  const std::size_t n = sym_count(d);
  const std::int64_t sy = v2o(d, d.symtab, n * 24);
  if (n && sy < 0) return pnr::Status::Fail(33, "module", "symbol table out of bounds");
  for (std::size_t i = 1; i < n; ++i) {
    const std::size_t o = static_cast<std::size_t>(sy) + i * 24;
    if (u16(b, o + 6) != 0) continue;
    std::string name;
    if (!sym_name(b, d, u32(b, o), &name)) return pnr::Status::Fail(34, "module", "symbol name out of bounds");
    if (name.empty()) continue;
    add_import(out, name, reg);
  }
  return pnr::Status::Ok();
}

ModuleImage::~ModuleImage() {
  if (base) ::munmap(base, span);
  for (int s : slots) release_slot(s);
}

// Guest-module export table: defined dynamic symbols of the modules loaded so far, in load
// order. Lookups walk it in REVERSE load order: a module's direct dependencies load after their
// own dependencies, so the module closest to the dependent wins a name conflict (matching the
// gABI global-scope order for a dependency chain; a diamond with the same symbol exported by two
// sibling modules resolves most-recent-first, a documented simplification of breadth-first).
using GuestExports = std::vector<std::pair<std::string, std::uint64_t>>;

pnr::Status load_one(const std::string& path, const pnr::prx::PrxRegistry& reg, const GuestExports* guest_exports,
                     std::unique_ptr<ModuleImage>* out, ModuleLoadReport* report) {
  auto fail = [](int c, const std::string& m) { return pnr::Status::Fail(c, "module", m); };
  ModuleLoadReport local;
  if (!report) report = &local;
  *report = ModuleLoadReport{};
  Bytes b;
  {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(40, "cannot read file: " + path);
    b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  Dyn d;
  auto st = parse_dyn(b, &d);
  if (!st) return st;

  std::uint64_t lo = ~0ull, hi = 0;
  for (const auto& s : d.loads) { lo = std::min(lo, s.vaddr); hi = std::max(hi, s.vaddr + s.memsz); }
  const std::uint64_t page = 4096;
  if (hi <= lo || hi - lo > (1ull << 30)) return fail(41, "unreasonable memory span");
  const std::uint64_t start = lo & ~(page - 1);
  const std::uint64_t span = ((hi + page - 1) & ~(page - 1)) - start;
  if (d.entry < lo || d.entry >= hi) return fail(42, "entry point outside the loaded segments");

  void* m = ::mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return fail(43, "mmap failed");
  auto img = std::make_unique<ModuleImage>();
  img->span = span;
  img->base = static_cast<std::uint8_t*>(m);
  const std::uint64_t bias = start;  // vaddr v lives at m + (v - bias)
  auto at = [&](std::uint64_t v) { return static_cast<std::uint8_t*>(m) + (v - bias); };
  for (const auto& s : d.loads) std::memcpy(at(s.vaddr), b.data() + s.off, s.filesz);
  // Convention for relocations: "load base" B such that B + vaddr is the mapped address.
  const std::uint64_t B = reinterpret_cast<std::uint64_t>(m) - bias;
  img->entry_vaddr = 0;

  auto in_img = [&](std::uint64_t v, std::uint64_t len) { return v >= start && len <= span && v - start <= span - len; };
  const std::size_t nsym = sym_count(d);
  const std::int64_t sy = d.symtab ? v2o(d, d.symtab, nsym * 24) : -1;

  struct Table { std::uint64_t addr, size; };
  const Table tables[2] = {{d.rela, d.relasz}, {d.jmprel, d.pltrelsz}};
  for (const auto& t : tables) {
    if (t.size == 0) continue;
    if (t.size % 24) return fail(44, "relocation table size is not a multiple of 24");
    const std::int64_t ro = v2o(d, t.addr, t.size);
    if (ro < 0) return fail(45, "relocation table out of bounds");
    for (std::uint64_t i = 0; i < t.size / 24; ++i) {
      const std::size_t o = static_cast<std::size_t>(ro) + i * 24;
      const std::uint64_t off = u64(b, o), info = u64(b, o + 8);
      const std::int64_t addend = static_cast<std::int64_t>(u64(b, o + 16));
      const std::uint32_t type = static_cast<std::uint32_t>(info & 0xffffffffu), sidx = static_cast<std::uint32_t>(info >> 32);
      if (!in_img(off, 8)) return fail(46, "relocation target outside the image");
      std::uint64_t value = 0;
      if (type == 8) {  // R_X86_64_RELATIVE
        value = B + static_cast<std::uint64_t>(addend);
      } else if (type == 1 || type == 6 || type == 7) {
        if (sidx == 0 || sidx >= nsym || sy < 0) return fail(47, "relocation names a bad symbol index");
        const std::size_t so = static_cast<std::size_t>(sy) + static_cast<std::size_t>(sidx) * 24;
        std::string name;
        if (!sym_name(b, d, u32(b, so), &name)) return fail(48, "symbol name out of bounds");
        const std::uint16_t shndx = u16(b, so + 6);
        if (shndx != 0) {
          value = B + u64(b, so + 8);  // defined inside this module
        } else {
          // Slice S33: bundled guest modules bind before the host PRX registry.
          const std::uint64_t* guest_value = nullptr;
          if (guest_exports)
            for (auto it = guest_exports->rbegin(); it != guest_exports->rend(); ++it)
              if (it->first == name) { guest_value = &it->second; break; }
          if (guest_value) {
            value = *guest_value;
            report->bound.push_back(name + " [guest module export]");
          } else {
            SonySymbol ss;
            const pnr::prx::PrxSymbol* sym = nullptr;
            if (parse_sony_symbol(name, &ss)) sym = reg.find(ss.nid); else sym = reg.find_name(name);
            const std::string label = (sym ? sym->name + " (" + (ss.nid.empty() ? sym->nid : ss.nid) + ")" : name);
            if (!sym || !sym->implemented || !sym->invoke) {
              report->missing.push_back(sym && !sym->implemented ? label + " [declared, not implemented]" : label + " [unknown]");
              continue;
            }
            const int slot = claim_slot(sym->invoke);
            if (slot < 0) return fail(49, "more than 64 bound imports in one process (current limit)");
            img->slots.push_back(slot);
            value = reinterpret_cast<std::uint64_t>(thunks()[slot]);
            report->bound.push_back(label);
          }
        }
        if (type == 1) value += static_cast<std::uint64_t>(addend);
      } else {
        return fail(50, "unsupported relocation type " + std::to_string(type));
      }
      std::memcpy(at(off), &value, 8);
      ++report->relocations;
    }
  }
  if (!report->missing.empty()) {
    std::string msg = "cannot bind " + std::to_string(report->missing.size()) + " import(s):";
    for (const auto& s : report->missing) msg += " " + s + ";";
    return fail(51, msg);
  }

  // Slice S32: lifecycle functions. Sizes must be whole pointers and every address must land
  // inside the image. Array entries are read AFTER relocation, so RELATIVE entries already hold
  // absolute addresses here; null entries are skipped (gABI permits them).
  if (d.init_arraysz % 8 || d.fini_arraysz % 8) return fail(54, "init/fini array size is not a multiple of 8");
  if (d.init_arraysz && !in_img(d.init_array, d.init_arraysz)) return fail(55, "DT_INIT_ARRAY outside the image");
  if (d.fini_arraysz && !in_img(d.fini_array, d.fini_arraysz)) return fail(56, "DT_FINI_ARRAY outside the image");
  if ((d.init && !in_img(d.init, 1)) || (d.fini && !in_img(d.fini, 1))) return fail(57, "DT_INIT/DT_FINI outside the image");
  if (d.init) img->init_fn = B + d.init;
  if (d.fini) img->fini_fn = B + d.fini;
  for (std::uint64_t i = 0; i < d.init_arraysz / 8; ++i) {
    std::uint64_t v = 0;
    std::memcpy(&v, at(d.init_array) + 8 * i, 8);
    if (v) img->init_array.push_back(v);
  }
  for (std::uint64_t i = 0; i < d.fini_arraysz / 8; ++i) {
    std::uint64_t v = 0;
    std::memcpy(&v, at(d.fini_array) + 8 * i, 8);
    if (v) img->fini_array.push_back(v);
  }

  // Page protections: the union of every segment touching a page.
  std::vector<int> prot(span / page, 0);
  for (const auto& s : d.loads) {
    int p = ((s.flags & 4) ? PROT_READ : 0) | ((s.flags & 2) ? PROT_WRITE : 0) | ((s.flags & 1) ? PROT_EXEC : 0);
    for (std::uint64_t a = s.vaddr & ~(page - 1); a < s.vaddr + s.memsz; a += page) prot[(a - start) / page] |= p;
  }
  for (std::size_t i = 0; i < prot.size(); ++i)
    if (::mprotect(static_cast<std::uint8_t*>(m) + i * page, page, prot[i] ? prot[i] : PROT_NONE) != 0) return fail(52, "mprotect failed");
  img->entry_vaddr = d.entry - start;
  if (d.has_tls) {
    if (!in_img(d.tls_vaddr, d.tls_memsz)) return fail(53, "PT_TLS outside the image");
    img->tls.init = at(d.tls_vaddr); img->tls.filesz = d.tls_filesz; img->tls.memsz = d.tls_memsz; img->tls.align = d.tls_align ? d.tls_align : 1;
  }
  // Slice S33: module identity (DT_SONAME), needed names and the export table used to bind
  // dependent modules. Exports are defined (st_shndx != 0), GLOBAL/WEAK dynamic symbols.
  if (d.soname && !sym_name(b, d, static_cast<std::uint32_t>(d.soname), &img->soname)) img->soname.clear();
  for (auto n : d.needed) {
    std::string s;
    if (sym_name(b, d, static_cast<std::uint32_t>(n), &s)) img->needed_names.push_back(s);
  }
  for (std::size_t i = 1; i < nsym; ++i) {
    const std::size_t o = static_cast<std::size_t>(sy) + i * 24;
    if (u16(b, o + 6) != 0 && (b[o + 4] >> 4) != 0) {  // defined, non-LOCAL
      std::string nm;
      if (sym_name(b, d, u32(b, o), &nm) && !nm.empty()) img->exports.emplace_back(nm, B + u64(b, o + 8));
    }
  }
  *out = std::move(img);
  return pnr::Status::Ok();
}

pnr::Status load_module(const std::string& path, const pnr::prx::PrxRegistry& reg, std::unique_ptr<ModuleImage>* out, ModuleLoadReport* report) {
  return load_one(path, reg, nullptr, out, report);
}

// Slice S33 helpers -----------------------------------------------------------

// Strips a PS module suffix (".prx"/".sprx", case-insensitive) for stem matching.
std::string module_stem(const std::string& name) {
  auto ends = [&](const char* suf) {
    const std::size_t n = std::strlen(suf);
    if (name.size() <= n) return false;
    for (std::size_t i = 0; i < n; ++i)
      if (std::tolower(static_cast<unsigned char>(name[name.size() - n + i])) != suf[i]) return false;
    return true;
  };
  if (ends(".prx") || ends(".sprx")) return name.substr(0, name.find_last_of('.'));
  return {};
}

// Finds the module file for a DT_NEEDED name: exact filename match first, then the PS stem
// convention ("libSceFoo.sprx" matches needed "libSceFoo"). Matching two different files is an
// ambiguity error. Returns an empty path when no file matches (firmware-provided).
pnr::Status find_module_file(const std::string& name, const std::vector<std::string>& dirs, std::string* found) {
  *found = {};
  const std::string stem = module_stem(name);
  std::string exact, stemmed;
  std::error_code ec;
  for (const auto& dir : dirs) {
    for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
      if (ec || !ent.is_regular_file(ec)) continue;
      const std::string fn = ent.path().filename().string();
      if (fn == name) {
        if (exact.empty()) exact = ent.path().string();
        else if (ent.path().string() != exact)
          return pnr::Status::Fail(70, "module", "ambiguous dependent module \"" + name + "\": " + exact + " and " + ent.path().string());
      } else if (!stem.empty() && module_stem(fn) == stem) {
        if (stemmed.empty()) stemmed = ent.path().string();
        else if (ent.path().string() != stemmed)
          return pnr::Status::Fail(70, "module", "ambiguous dependent module \"" + name + "\": " + stemmed + " and " + ent.path().string());
      }
    }
  }
  *found = !exact.empty() ? exact : stemmed;
  return pnr::Status::Ok();
}

// Recursive loader state: post-order over DT_NEEDED files, cycle-safe.
struct TreeLoad {
  const pnr::prx::PrxRegistry* reg;
  std::vector<std::string> dirs;
  std::unique_ptr<ModuleTree> tree = std::make_unique<ModuleTree>();
  GuestExports exports;  // all loaded modules so far, load order
  std::map<std::string, bool> visiting;  // canonical path -> still on the DFS stack
  std::set<std::string> done;
};

pnr::Status load_recursive(const std::string& path, bool is_main, TreeLoad* t, ModuleLoadReport* report) {
  auto fail = [](int c, const std::string& m) { return pnr::Status::Fail(c, "module", m); };
  std::error_code ec;
  const std::string canon = std::filesystem::weakly_canonical(path, ec).string();
  const std::string& key = ec ? path : canon;
  if (t->done.count(key)) return pnr::Status::Ok();
  if (t->visiting[key]) return fail(71, "cyclic module dependency involving " + key);
  if (t->tree->images.size() >= 32) return fail(72, "more than 32 modules in one tree (current limit)");
  t->visiting[key] = true;

  // Read + parse once here for the needed names; load_one re-parses (cheap, bounds-checked).
  std::vector<std::uint8_t> b;
  {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(40, "cannot read file: " + path);
    b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  Dyn d;
  auto st = parse_dyn(b, &d);
  if (!st) return st;
  const std::int64_t so = (d.has_dyn && d.strtab) ? v2o(d, d.strtab, d.strsz) : -1;
  if (d.has_dyn && d.strtab && so < 0) return fail(32, "string table out of bounds");

  std::vector<std::string> dep_files;
  for (auto n : d.needed) {
    std::string name;
    if (so >= 0 && !sym_name(b, d, static_cast<std::uint32_t>(n), &name)) return fail(34, "symbol name out of bounds");
    if (name.empty()) continue;
    std::string file;
    st = find_module_file(name, t->dirs, &file);
    if (!st) return st;
    if (!file.empty()) {
      // Load the dependency before this module (post-order).
      st = load_recursive(file, false, t, report);
      if (!st)
        return fail(73, "dependent module \"" + name + "\" (" + file + ") needed by " + path + ": " + st.error.message);
      dep_files.push_back(file);
    }
    // No matching file: firmware-provided module, bound through the host PRX registry below.
  }

  std::unique_ptr<ModuleImage> img;
  st = load_one(path, *t->reg, &t->exports, &img, report);
  if (!st) return st;
  if (!is_main && img->tls.init)
    return fail(74, "dependent module " + path + " has PT_TLS; only the main module's TLS is set up (current limit)");
  t->exports.insert(t->exports.end(), img->exports.begin(), img->exports.end());
  t->tree->images.push_back(std::move(img));
  t->visiting[key] = false;
  t->done.insert(key);
  return pnr::Status::Ok();
}

pnr::Status load_module_tree(const std::string& main_path, const std::vector<std::string>& module_dirs,
                             const pnr::prx::PrxRegistry& reg, std::unique_ptr<ModuleTree>* out, ModuleLoadReport* report) {
  TreeLoad t;
  t.reg = &reg;
  t.dirs = module_dirs;
  // Console convention: the module's own directory and its module subdirectories are searched.
  const std::filesystem::path base = std::filesystem::path(main_path).parent_path();
  t.dirs.push_back(base.empty() ? "." : base.string());
  for (const char* sub : {"sce_module", "sce_modules", "prx"}) t.dirs.push_back((base / sub).string());
  ModuleLoadReport local;
  if (!report) report = &local;
  *report = ModuleLoadReport{};
  auto st = load_recursive(main_path, true, &t, report);
  if (!st) return st;
  *out = std::move(t.tree);
  return pnr::Status::Ok();
}

pnr::Status run_module(const std::string& path, const pnr::prx::PrxRegistry& reg, ModuleRunResult* result, ModuleLoadReport* report) {
  return run_module_tree(path, {}, reg, result, report);
}

pnr::Status run_module_tree(const std::string& main_path, const std::vector<std::string>& module_dirs,
                            const pnr::prx::PrxRegistry& reg, ModuleRunResult* result, ModuleLoadReport* report) {
  *result = ModuleRunResult{};
  std::unique_ptr<ModuleTree> tree;
  auto st = load_module_tree(main_path, module_dirs, reg, &tree, report);
  if (!st) return st;
  ModuleImage* img = tree->main();
  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t pid = ::fork();
  if (pid < 0) return pnr::Status::Fail(60, "module", "fork failed");
  if (pid == 0) {
    // PlayStation-style entry: entry(args_block, exit_handler). The result is the process status.
    // S32/S33: dependencies' DT_INIT+DT_INIT_ARRAY run first (load order), then the main
    // module's; on exit the atexit handlers run LIFO (one global list - a real dynamic loader
    // interleaves them per module, a documented simplification), then every module's
    // DT_FINI_ARRAY (reverse)+DT_FINI in exact reverse init order.
    auto args = pnr::loader::build_entry_args({main_path});
    pnr::loader::GuestLifecycle lc;
    for (const auto& m : tree->images) {
      if (m->init_fn) lc.init.push_back(m->init_fn);
      for (const std::uint64_t v : m->init_array) lc.init.push_back(v);
    }
    for (auto mit = tree->images.rbegin(); mit != tree->images.rend(); ++mit) {
      for (auto it = (*mit)->fini_array.rbegin(); it != (*mit)->fini_array.rend(); ++it) lc.fini.push_back(*it);
      if ((*mit)->fini_fn) lc.fini.push_back((*mit)->fini_fn);
    }
    using EntryFn = std::int64_t (*)(void*, void*);
    const std::int64_t rc = pnr::loader::run_guest_main(img->tls, reinterpret_cast<EntryFn>(img->entry()), args.data(),
                                                        reinterpret_cast<void*>(&pnr_guest_exit), lc);
    std::fflush(stdout);
    ::_exit(static_cast<int>(rc) & 0xff);
  }
  result->started = true;
  int ws = 0;
  if (::waitpid(pid, &ws, 0) < 0) return pnr::Status::Fail(61, "module", "waitpid failed");
  result->finished = true;
  if (WIFEXITED(ws)) result->exit_code = WEXITSTATUS(ws);
  if (WIFSIGNALED(ws)) { result->signaled = true; result->signal = WTERMSIG(ws); }
  return pnr::Status::Ok();
}

}  // namespace pnr::loader
