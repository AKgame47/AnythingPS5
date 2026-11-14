// Core slice C4c tests: GPU virtual address map on SYNTHETIC data.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "graphics/gpu_va.hpp"
#include "graphics/pm4.hpp"

void pnr_gpu_va_tests(int& pass, int& fail) {
  using namespace pnr::graphics;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  GpuAddressSpace as;
  std::vector<std::uint32_t> host = {1, 2, 3, 4};
  check(as.map_external(0x1000, host.data(), 16, "a").empty() && as.region_count() == 1, "va: external mapping accepted");
  const auto* p = as.resolve(0x1004, 8);
  check(p && std::memcmp(p, &host[1], 8) == 0, "va: resolve inside a region gives the host bytes at the right offset");
  check(as.resolve(0x1008, 16) == nullptr && as.last_error().find("past the end") != std::string::npos, "va: a range that runs past the end is refused, with a reason");
  check(as.resolve(0x0fff, 1) == nullptr && as.resolve(0x1010, 1) == nullptr && as.last_error().find("not mapped") != std::string::npos, "va: addresses before and after the region are not mapped");
  check(as.resolve(0x100f, 1) != nullptr && as.resolve(0x1000, 16) != nullptr, "va: first and last byte resolve");
  check(!as.map_external(0x1008, host.data(), 8, "b").empty() && !as.map_external(0xff8, host.data(), 16, "c").empty() && !as.map_external(0x1000, host.data(), 16, "d").empty(), "va: overlapping mappings are refused (tail, head, exact)");
  check(as.map_external(0x1010, host.data(), 16, "adjacent").empty(), "va: an adjacent mapping is allowed");
  check(!as.map_external(0, host.data(), 0, "z").empty() && !as.map_external(~0ull - 3, host.data(), 16, "w").empty() && !as.map_external(0x9000, nullptr, 4, "n").empty(), "va: zero size, wrap-around and null host are refused");
  check(as.resolve(0x100c, 8) == nullptr, "va: a range spanning two adjacent mappings is refused (no stitching)");
  std::uint32_t v = 0;
  check(as.read(0x1008, &v, 4) && v == 3 && as.write(0x1008, &(v = 99), 4) && host[2] == 99, "va: read and write go through to host memory");
  std::uint8_t src[4] = {9, 8, 7, 6};
  check(as.map_copy(0x2000, src, 4, "copy").empty(), "va: map_copy accepted");
  src[0] = 0;
  check(as.resolve(0x2000, 1)[0] == 9, "va: map_copy owns its own bytes");
  check(as.unmap(0x2000) && !as.unmap(0x2000) && as.resolve(0x2000, 1) == nullptr, "va: unmap removes the mapping once");
  check(as.describe().find("adjacent") != std::string::npos, "va: describe lists regions");

  // PM4 indirect buffer through the resolver.
  pm4::Builder inner; inner.set_context(0xa010, 0x77).draw_auto(3);
  std::vector<std::uint32_t> ib = inner.dwords();
  GpuAddressSpace as2;
  as2.map_external(0x5000, ib.data(), ib.size() * 4, "ib");
  pm4::Builder outer; outer.indirect(0x5000, static_cast<std::uint32_t>(ib.size())).draw_auto(9);
  auto s = pm4::interpret(outer.dwords().data(), outer.dwords().size(), as2.pm4_resolver());
  check(s.draws.size() == 2 && s.draws[0].count == 3 && s.draws[1].count == 9 && s.indirect_unresolved == 0, "va: PM4 indirect buffer is followed through the address map");
  GpuAddressSpace empty;
  auto s2 = pm4::interpret(outer.dwords().data(), outer.dwords().size(), empty.pm4_resolver());
  check(s2.indirect_unresolved == 1 && s2.draws.size() == 1, "va: an unmapped indirect buffer is counted as unresolved, not guessed");
  auto r = as2.pm4_resolver();
  check(r(0x5002, 1) == nullptr && as2.last_error().find("aligned") != std::string::npos, "va: unaligned indirect address refused");
}
