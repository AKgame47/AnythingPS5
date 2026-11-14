// pnr_make_ps_elf: writes a synthetic PlayStation-convention test ELF (not a real game).
#include <cstdio>
#include <cstring>
#include <fstream>

#include "tools/synth_elf.hpp"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: pnr_make_ps_elf OUT.elf [--missing-import]\n"); return 64; }
  pnr::tools::SynthSpec spec;
  for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--missing-import")) spec.include_missing_import = true;
  const auto b = pnr::tools::build_synthetic_ps_elf(spec);
  std::ofstream f(argv[1], std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
  std::printf("wrote %s (synthetic test module, %zu bytes)\n", argv[1], b.size());
  return 0;
}
