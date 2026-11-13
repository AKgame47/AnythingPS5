#include "loader/import_resolver.hpp"

#include <cstring>
#include <fstream>

#include "loader/nid.hpp"

namespace pnr::loader {
namespace {

bool read_file(const std::string& path, std::vector<std::uint8_t>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const auto n = f.tellg();
  if (n < 0) return false;
  f.seekg(0, std::ios::beg);
  out->resize(static_cast<std::size_t>(n));
  if (n > 0) f.read(reinterpret_cast<char*>(out->data()), n);
  return static_cast<bool>(f) || n == 0;
}

std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t o) {
  return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t o) {
  return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
         (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
std::uint64_t u64(const std::vector<std::uint8_t>& b, std::size_t o) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[o + i]) << (8 * i);
  return v;
}

bool in(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t len) {
  return off <= b.size() && len <= b.size() - off;
}

std::string cstr(const std::vector<std::uint8_t>& b, std::size_t base, std::size_t limit) {
  if (base >= limit) return {};
  std::string s;
  for (std::size_t i = base; i < limit && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
  return s;
}

}  // namespace

const char* import_class_name(ImportClass c) {
  switch (c) {
    case ImportClass::Host: return "host";
    case ImportClass::PrxImplemented: return "prx-implemented";
    case ImportClass::PrxDeclared: return "prx-declared";
    case ImportClass::Unknown: return "unknown";
    case ImportClass::PnrExtension: return "pnr-extension";
  }
  return "?";
}

pnr::Status resolve_imports(const std::string& path,
                            const pnr::prx::PrxRegistry& reg,
                            ImportResolution* out) {
  if (out == nullptr)
    return pnr::Status::Fail(1, "loader", "resolve_imports: null out");
  *out = ImportResolution{};
  std::vector<std::uint8_t> b;
  if (!read_file(path, &b))
    return pnr::Status::Fail(2, "loader", "cannot read file: " + path);
  if (!in(b, 0, 64) || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
    return pnr::Status::Fail(3, "loader", "not an ELF file: " + path);
  if (b[4] != 2) return pnr::Status::Fail(4, "loader", "not a 64-bit ELF: " + path);

  const std::uint64_t shoff = u64(b, 0x28);
  const std::uint16_t shentsize = u16(b, 0x3A);
  const std::uint16_t shnum = u16(b, 0x3C);
  if (shoff == 0 || shnum == 0) return pnr::Status::Ok();  // no sections
  if (shentsize < 64 || !in(b, shoff, static_cast<std::size_t>(shentsize) * shnum))
    return pnr::Status::Fail(5, "loader", "section headers out of bounds: " + path);

  struct Shdr { std::uint32_t type; std::uint64_t off, size; std::uint32_t link; };
  std::vector<Shdr> sh(shnum);
  for (std::size_t i = 0; i < shnum; ++i) {
    const std::size_t o = shoff + i * shentsize;
    sh[i] = Shdr{u32(b, o + 4), u64(b, o + 24), u64(b, o + 32), u32(b, o + 40)};
  }

  // DT_NEEDED via SHT_DYNAMIC (6), strings via its linked section.
  for (std::size_t i = 0; i < shnum; ++i) {
    if (sh[i].type != 6) continue;
    if (sh[i].link >= shnum) return pnr::Status::Fail(6, "loader", "bad dynamic link");
    const auto& strs = sh[sh[i].link];
    if (!in(b, strs.off, strs.size) || !in(b, sh[i].off, sh[i].size))
      return pnr::Status::Fail(7, "loader", "dynamic section out of bounds");
    for (std::size_t o = sh[i].off; o + 16 <= sh[i].off + sh[i].size; o += 16) {
      const std::int64_t tag = static_cast<std::int64_t>(u64(b, o));
      if (tag == 0) break;
      if (tag == 1) {  // DT_NEEDED
        const std::uint64_t so = strs.off + u64(b, o + 8);
        if (so < strs.off + strs.size)
          out->needed_libs.push_back(cstr(b, so, strs.off + strs.size));
      }
    }
  }

  // Undefined symbols via SHT_DYNSYM (11), strings via its linked section.
  for (std::size_t i = 0; i < shnum; ++i) {
    if (sh[i].type != 11) continue;
    if (sh[i].link >= shnum) return pnr::Status::Fail(8, "loader", "bad dynsym link");
    const auto& strs = sh[sh[i].link];
    if (!in(b, strs.off, strs.size) || !in(b, sh[i].off, sh[i].size))
      return pnr::Status::Fail(9, "loader", "dynsym out of bounds");
    for (std::size_t o = sh[i].off; o + 24 <= sh[i].off + sh[i].size; o += 24) {
      if (u16(b, o + 6) != 0) continue;       // st_shndx != SHN_UNDEF
      const std::uint32_t noff = u32(b, o);    // st_name
      if (noff == 0) continue;
      const std::uint64_t so = strs.off + noff;
      if (so >= strs.off + strs.size)
        return pnr::Status::Fail(10, "loader", "symbol name out of bounds");
      ImportEntry e{cstr(b, so, strs.off + strs.size), ImportClass::Host};
      const pnr::prx::PrxSymbol* sym = reg.find_name(e.name);
      if (sym != nullptr) {
        e.klass = sym->implemented ? ImportClass::PrxImplemented
                                   : ImportClass::PrxDeclared;
      } else if (e.name.rfind("pnr", 0) == 0) {
        e.klass = ImportClass::PnrExtension;
      } else if (e.name.rfind("sce", 0) == 0) {
        e.klass = ImportClass::Unknown;  // PS-style but outside taxonomy
      }
      switch (e.klass) {
        case ImportClass::Host: ++out->host; break;
        case ImportClass::PrxImplemented: ++out->prx_implemented; break;
        case ImportClass::PrxDeclared: ++out->prx_declared; break;
        case ImportClass::Unknown: ++out->unknown; break;
        case ImportClass::PnrExtension: ++out->pnr_extension; break;
      }
      out->imports.push_back(std::move(e));
    }
  }
  return pnr::Status::Ok();
}

}  // namespace pnr::loader
