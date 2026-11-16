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
//  - C2 added: PT_TLS setup (variant II TCB), entry(args, exit_handler), guest FS switching
//    around host calls.
//  - S32 added: DT_INIT/DT_INIT_ARRAY run before entry; atexit handlers (LIFO), DT_FINI_ARRAY
//    (reverse) and DT_FINI run on exit.
//  - S33 added: dependent modules. A DT_NEEDED name matching a module file found in the search
//    directories (the main module's directory, its sce_module/sce_modules/prx subdirectories,
//    plus any directories the caller adds) is loaded as a guest module, recursively, before the
//    module that needs it. Bundled modules take precedence over the host PRX registry (console
//    semantics, same as AnyPS5's relinker); a needed name with no matching file is assumed
//    firmware-provided and stays host-bound. Limits: dependent modules with PT_TLS are refused,
//    at most 32 modules per tree, cyclic dependencies are refused, and a name matching two
//    different files is an ambiguity error. Symbol exports are resolved first-loaded-wins.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/runtime.hpp"
#include "loader/guest_runtime.hpp"
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
  TlsTemplate tls;
  std::vector<int> slots;  // thunk slots held by this image
  // Slice S32 lifecycle: absolute addresses (0 = absent). Array contents are read AFTER
  // relocation, so RELATIVE entries in the file are already absolute here. File order kept.
  std::uint64_t init_fn = 0, fini_fn = 0;
  std::vector<std::uint64_t> init_array, fini_array;
  // Slice S33: module identity and exports for dependent-module binding.
  std::string soname;                             // DT_SONAME (empty when absent)
  std::vector<std::string> needed_names;          // DT_NEEDED strings, file order
  std::vector<std::pair<std::string, std::uint64_t>> exports;  // defined dynamic symbols, absolute
  std::uint8_t* entry() const { return base + entry_vaddr; }
};

// Slice S33: a loaded module tree. images holds dependencies first (post-order), main module last.
struct ModuleTree {
  std::vector<std::unique_ptr<ModuleImage>> images;
  ModuleImage* main() const { return images.empty() ? nullptr : images.back().get(); }
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

// Loads, then runs the entry point `int entry(void* args, void (*exit)(void))` in a forked child.
pnr::Status run_module(const std::string& path, const pnr::prx::PrxRegistry& reg, ModuleRunResult* result,
                       ModuleLoadReport* report);

// Slice S33: loads the main module and every dependent module named by its DT_NEEDED entries
// (recursively) that exists as a file in the search directories. Search order: caller's
// module_dirs first, then the main module's own directory, then its sce_module, sce_modules and
// prx subdirectories. A needed name matches a file by exact name or by name without a .prx/.sprx
// suffix (PS module stem convention); matching two files is an ambiguity error. Bundled guest
// modules are bound BEFORE the host PRX registry. Dependencies' init runs before the dependent's,
// fini in exact reverse order; atexit handlers still run first, as one global LIFO list (a real
// dynamic loader interleaves them per module; PNR does not track the dso handle, so this
// ordering is a documented simplification).
pnr::Status load_module_tree(const std::string& main_path, const std::vector<std::string>& module_dirs,
                             const pnr::prx::PrxRegistry& reg, std::unique_ptr<ModuleTree>* out, ModuleLoadReport* report);

// Loads the tree, then runs the main module's entry point in a forked child.
pnr::Status run_module_tree(const std::string& main_path, const std::vector<std::string>& module_dirs,
                            const pnr::prx::PrxRegistry& reg, ModuleRunResult* result, ModuleLoadReport* report);

}  // namespace pnr::loader
