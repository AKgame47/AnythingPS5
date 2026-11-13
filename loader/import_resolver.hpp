// PNR Loader - import resolution (part of MODULE 02 + PRX layer)
// Reads an ELF binary's dynamic imports (DT_NEEDED + undefined .dynsym
// symbols) and classifies each against the PRX system-library taxonomy.
// Native host libraries stay host-resolved; PS-style symbols are bound
// (or honestly reported) through PrxRegistry. Nothing is faked: a symbol
// we cannot satisfy is reported, not hidden.
// Status: Implemented, Tested.
#pragma once
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "prx/prx.hpp"

namespace pnr::loader {

enum class ImportClass {
  Host,           // satisfied by the host's native libraries
  PrxImplemented, // in the PRX taxonomy with a working host implementation
  PrxDeclared,    // in the PRX taxonomy but NOT implemented (honest)
  Unknown,        // PS-style name (sce*) outside the taxonomy
  PnrExtension,   // pnr* call provided by the PNR shim (not a Sony API)
};

struct ImportEntry {
  std::string name;
  ImportClass klass = ImportClass::Host;
};

struct ImportResolution {
  std::vector<std::string> needed_libs;  // DT_NEEDED entries
  std::vector<ImportEntry> imports;      // undefined dynamic symbols
  int host = 0, prx_implemented = 0, prx_declared = 0, unknown = 0,
      pnr_extension = 0;
};

// Parses the ELF's dynamic section/symbol table (bounds-checked) and
// classifies every undefined symbol. Fails with a specific Status error
// for malformed files. Binaries without a dynamic section resolve empty.
pnr::Status resolve_imports(const std::string& path,
                            const pnr::prx::PrxRegistry& reg,
                            ImportResolution* out);

const char* import_class_name(ImportClass c);

}  // namespace pnr::loader
