// pnr_make_ps_elf: writes a synthetic PlayStation-convention test ELF (not a real game).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "tools/synth_elf.hpp"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: pnr_make_ps_elf OUT.elf [--missing-import|--kernel|--kernel-no-tls]\n"); return 64; }
  pnr::tools::SynthSpec spec;
  for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--missing-import")) spec.include_missing_import = true;
  std::vector<std::uint8_t> b;
  bool kernel = false;
  pnr::tools::KernelSpec ks;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--kernel")) kernel = true;
    if (!std::strcmp(argv[i], "--kernel-no-tls")) { kernel = true; ks.break_tls = true; }
  }
  b = kernel ? pnr::tools::build_synthetic_kernel_elf(ks) : pnr::tools::build_synthetic_ps_elf(spec);
  std::ofstream f(argv[1], std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
  std::printf("wrote %s (synthetic test module, %zu bytes)\n", argv[1], b.size());
  return 0;
}
