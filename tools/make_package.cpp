// Dev tool: build a .pnrgame from a manifest + payload files.
// usage: pnr_make_package <out.pnrgame> <id> <title> <version> <exe-pkg-path> <pkg-path=host-path>...
// The executable's package path must equal <exe-pkg-path>.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "package/importer.hpp"

int main(int argc, char** argv) {
  if (argc < 7) {
    std::fprintf(stderr,
                 "usage: %s <out.pnrgame> <id> <title> <version> "
                 "<exe-pkg-path> <pkg=host>...\n",
                 argv[0]);
    return 64;
  }
  pnr::pkg::Manifest m;
  m.id = argv[2];
  m.title = argv[3];
  m.version = argv[4];
  m.executable = argv[5];
  std::vector<std::pair<std::string, std::string>> files;
  for (int i = 6; i < argc; ++i) {
    std::string spec = argv[i];
    auto eq = spec.find('=');
    if (eq == std::string::npos) {
      std::fprintf(stderr, "bad file spec (want pkg=host): %s\n",
                   spec.c_str());
      return 64;
    }
    files.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
  }
  auto st = pnr::pkg::build_package(argv[1], m, files);
  if (!st) {
    std::fprintf(stderr, "build failed: %s\n", st.error.message.c_str());
    return 1;
  }
  std::printf("built %s (id=%s version=%s)\n", argv[1], m.id.c_str(),
              m.version.c_str());
  return 0;
}
