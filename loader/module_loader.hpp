// PNR Loader - in-process module loader for Sony-style ELF files (Core slice C1), GPL-2.0.
// Maps PT_LOAD segments, applies x86-64 relocations, and binds NID-named imports
// to PRX host functions. Imported code is only MAPPED in the launcher process;
// it is EXECUTED in a forked child (run_module).
//
// Status: Implemented, Tested with SYNTHETIC ELF files built by tools/synth_elf.
// NOT tested on any real PlayStation executable. Honest limits:
//  - plain (already unprotected) ELF only. A SELF container is detected and refused;
//    PNR never decrypts or removes protection.
//  - relocations: RELATIVE, 64, GLOB_DAT, JUMP_SLOT. Anything else is refused.
//  - up to 64 bound imports per process, each called with up to 6 integer arguments.
//    No floating point arguments, no varargs beyond the 6 registers.
//  - no TLS setup, no init/fini arrays, no dependent modules yet (later slices).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "loader/import_resolver.hpp"
#include "prx/prx.hpp"

namespace pnr::loader {

class ModuleImage {
 public:
  ~ModuleImage();
  ModuleImage(const ModuleImage&) = delete;
  ModuleImage& operator=(const ModuleImage&) = delete;
  ModuleImage() = default;
  std::uint8_t* base = nullptr;
  std::size_t span = 0;
  std::uint64_t entry_vaddr = 0;
  std::vector<int> slots;  // thunk slots held by this image
  std::uint8_t* entry() const { return base + entry_vaddr; }
};

struct ModuleLoadReport {
  int relocations = 0;
  std::vector<std::string> bound;    // "name (NID)" for each import bound
  std::vector<std::string> missing;  // imports that could not be bound
};

struct ModuleRunResult {
  bool started = false, finished = false, signaled = false;
  int exit_code = -1, signal = 0;
};

bool is_self_container(const std::vector<std::uint8_t>& bytes);

// Lists the imports of an ELF without section headers by walking PT_DYNAMIC.
pnr::Status resolve_imports_from_program_headers(const std::vector<std::uint8_t>& bytes,
                                                 const pnr::prx::PrxRegistry& reg, ImportResolution* out);

pnr::Status load_module(const std::string& path, const pnr::prx::PrxRegistry& reg,
                        std::unique_ptr<ModuleImage>* out, ModuleLoadReport* report);

// Loads, then runs the entry point `int entry(void)` in a forked child.
pnr::Status run_module(const std::string& path, const pnr::prx::PrxRegistry& reg, ModuleRunResult* result,
                       ModuleLoadReport* report);

}  // namespace pnr::loader
