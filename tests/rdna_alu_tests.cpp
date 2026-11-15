// Slice S5 tests: wider RDNA float ALU coverage in the translator.
// v_subrev_f32 (VOP2 5), v_min_f32 (15), v_max_f32 (16), v_mac_f32 (31),
// v_rcp_f32 (VOP1 42), v_sqrt_f32 (VOP1 51), v_cvt_f32_i32 (5),
// v_cvt_i32_f32 (8) - opcode numbers from AMD's public "RDNA 2" ISA
// (VOP1/VOP2 tables). Synthetic kernels on the S2 vertex layout; real
// translation, spirv-val when configured, lavapipe execution and host
// readback compared against a CPU float reference. No commercial shaders.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "graphics/rdna_vertex.hpp"
#include "graphics/vulkan.hpp"
#include "shader/rdna.hpp"
#include "shader/translate.hpp"

namespace {

using pnr::graphics::VulkanBackend;
using pnr::graphics::detail::rdna_vop2;
using pnr::graphics::detail::rdna_prelude;
using pnr::graphics::detail::rdna_epilogue;
using pnr::graphics::kRdnaVertexBase;

uint32_t vop1(uint32_t op, uint32_t d, uint32_t src0) {
  return 0x7e000000u | (op << 9u) | (d << 17u) | src0;
}

// Kernel: load regions x->v11, y->v12, z->v13 (prelude), one ALU op into
// v14, store v14 into the out region (epilogue). 24 lanes, 96 buffer words.
std::vector<uint32_t> alu_kernel(const std::vector<uint32_t>& body) {
  std::vector<uint32_t> w;
  rdna_prelude(w, 24);
  w.insert(w.end(), body.begin(), body.end());
  rdna_epilogue(w);
  return w;
}

float as_float(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
uint32_t as_u32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

}  // namespace

void pnr_rdna_alu_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& l) {
    if (ok) ++pass; else ++fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str());
  };

  // ---- decode/translate acceptance (host-only) ----
  const std::vector<std::pair<std::string, std::vector<uint32_t>>> kernels = {
    {"subrev", {rdna_vop2(0x05, 14, 12, 256 + 11)}},
    {"min",    {rdna_vop2(0x0f, 14, 12, 256 + 11)}},
    {"max",    {rdna_vop2(0x10, 14, 12, 256 + 11)}},
    {"mac",    {vop1(1, 14, 256 + 13), rdna_vop2(0x1f, 14, 12, 256 + 11)}},
    {"rcp",    {vop1(0x2a, 14, 256 + 11)}},
    {"sqrt",   {vop1(0x33, 14, 256 + 11)}},
    {"f2i",    {vop1(0x05, 14, 256 + 11)}},
    {"i2f",    {vop1(0x08, 14, 256 + 11)}},
  };
  for (const auto& [name, body] : kernels) {
    auto w = alu_kernel(body);
    auto d = pnr::shader::rdna_decode(w);
    auto t = pnr::shader::translate_compute_words(w, kRdnaVertexBase, 96, 24, 0);
    check(d.ok && t.ok && t.host_float32, "rdna-alu: " + name + " kernel decodes and translates");
  }
  {
    // v_mac_f32 with an undefined accumulator must be refused, not invented.
    auto w = alu_kernel({rdna_vop2(0x1f, 20, 12, 256 + 11)});  // v20 never written
    check(!pnr::shader::translate_compute_words(w, kRdnaVertexBase, 96, 24, 0).ok,
          "rdna-alu: mac with undefined accumulator rejected");
  }

  // ---- execution on the real Vulkan backend (llvmpipe here) ----
  VulkanBackend vk;
  auto st = vk.open();
  if (!st) { std::printf("SKIP rdna-alu exec: %s\n", st.error.message.c_str()); return; }
  st = vk.create_instance("pnr-rdna-alu-tests");
  if (!st || vk.devices().empty()) { check(false, "rdna-alu: device available"); return; }
  st = vk.create_device(0);
  check(bool(st), "rdna-alu: device created");
  if (!st) return;

  const uint32_t N = 24;
  std::vector<float> in0(N), in1(N), acc(N);
  for (uint32_t i = 0; i < N; ++i) {
    in0[i] = float(i + 1) * 0.5f;                 // 0.5 .. 12.0 (exact)
    in1[i] = float(int(i % 7) - 3) * 1.25f;       // -3.75 .. 3.75 (exact)
    acc[i] = float(i) * 0.25f + 1.0f;             // 1.0 .. 6.75 (exact)
  }
  auto run = [&](const std::vector<uint32_t>& body, const std::vector<uint32_t>& region0,
                 std::vector<uint32_t>* out_words) -> bool {
    auto w = alu_kernel(body);
    auto t = pnr::shader::translate_compute_words(w, kRdnaVertexBase, 96, 24, 0);
    if (!t.ok) return false;
    std::vector<uint32_t> io(96, 0);
    for (uint32_t i = 0; i < N; ++i) {
      io[i] = region0[i];
      io[24 + i] = as_u32(in1[i]);
      io[48 + i] = as_u32(acc[i]);
    }
    st = vk.run_compute_words(t.spirv, &io, {});
    if (!st) { std::printf("    rdna-alu dispatch failed: %s\n", st.error.message.c_str()); return false; }
    out_words->assign(io.begin() + 72, io.end());
    return true;
  };
  std::vector<uint32_t> r0(N);
  for (uint32_t i = 0; i < N; ++i) r0[i] = as_u32(in0[i]);

  auto expect_float = [&](const char* name, const std::vector<uint32_t>& body,
                          const std::vector<float>& ref, uint32_t max_ulp) {
    std::vector<uint32_t> out;
    if (!run(body, r0, &out)) { check(false, std::string("rdna-alu: ") + name + " executes"); return; }
    uint32_t worst = 0;
    for (uint32_t i = 0; i < N; ++i) {
      const uint32_t a = out[i], b = as_u32(ref[i]);
      const uint32_t diff = a > b ? a - b : b - a;
      if (diff > worst) worst = diff;
    }
    check(worst <= max_ulp, std::string("rdna-alu: ") + name + " matches CPU reference (max ulp " +
                            std::to_string(worst) + ")");
  };

  std::vector<float> ref(N);
  for (uint32_t i = 0; i < N; ++i) ref[i] = in0[i] - in1[i];
  expect_float("v_sub_f32 still exact", {rdna_vop2(0x04, 14, 12, 256 + 11)}, ref, 0);
  for (uint32_t i = 0; i < N; ++i) ref[i] = in1[i] - in0[i];
  expect_float("v_subrev_f32 exact", {rdna_vop2(0x05, 14, 12, 256 + 11)}, ref, 0);
  for (uint32_t i = 0; i < N; ++i) ref[i] = std::fmin(in0[i], in1[i]);
  expect_float("v_min_f32 exact", {rdna_vop2(0x0f, 14, 12, 256 + 11)}, ref, 0);
  for (uint32_t i = 0; i < N; ++i) ref[i] = std::fmax(in0[i], in1[i]);
  expect_float("v_max_f32 exact", {rdna_vop2(0x10, 14, 12, 256 + 11)}, ref, 0);
  for (uint32_t i = 0; i < N; ++i) { const float m = in0[i] * in1[i]; ref[i] = m + acc[i]; }  // two roundings, not fused
  expect_float("v_mac_f32 mul-then-add exact", {vop1(1, 14, 256 + 13), rdna_vop2(0x1f, 14, 12, 256 + 11)}, ref, 0);
  for (uint32_t i = 0; i < N; ++i) ref[i] = 1.0f / in0[i];
  expect_float("v_rcp_f32 as exact division", {vop1(0x2a, 14, 256 + 11)}, ref, 1);
  for (uint32_t i = 0; i < N; ++i) ref[i] = std::sqrt(in0[i]);
  expect_float("v_sqrt_f32 <= 1 ulp", {vop1(0x33, 14, 256 + 11)}, ref, 1);

  {
    // v_cvt_f32_i32: truncation toward zero, negatives included.
    std::vector<uint32_t> out;
    bool ok = run({vop1(0x05, 14, 256 + 12)}, r0, &out);  // convert region y (-3.75..3.75)
    if (ok) for (uint32_t i = 0; i < N && ok; ++i)
      ok = int32_t(out[i]) == int32_t(in1[i]);  // C++ cast truncates toward zero
    check(ok, "rdna-alu: v_cvt_f32_i32 truncates like the CPU reference");
  }
  {
    // v_cvt_i32_f32: small integers convert exactly.
    std::vector<uint32_t> ints(N);
    for (uint32_t i = 0; i < N; ++i) ints[i] = uint32_t(int32_t(i) - 12);
    std::vector<uint32_t> out;
    bool ok = run({vop1(0x08, 14, 256 + 11)}, ints, &out);
    if (ok) for (uint32_t i = 0; i < N && ok; ++i)
      ok = as_float(out[i]) == float(int32_t(i) - 12);
    check(ok, "rdna-alu: v_cvt_i32_f32 exact for small integers");
  }

  // spirv-val on one mixed kernel when configured.
  if (const char* val = std::getenv("PNR_SPIRV_VAL")) {
    auto w = alu_kernel({rdna_vop2(0x0f, 14, 12, 256 + 11),
                         vop1(0x2a, 14, 256 + 14),
                         vop1(0x05, 14, 256 + 14)});
    auto t = pnr::shader::translate_compute_words(w, kRdnaVertexBase, 96, 24, 0);
    if (!t.ok) { check(false, "rdna-alu: mixed min/rcp/cvt kernel translates"); return; }
    check(true, "rdna-alu: mixed min/rcp/cvt kernel translates");
    const std::string tmp = "/tmp/pnr_alu_" + std::to_string(::getpid()) + ".spv";
    { std::ofstream o(tmp, std::ios::binary); o.write(reinterpret_cast<const char*>(t.spirv.data()), t.spirv.size() * 4); }
    const std::string cmd = std::string(val) + " " + tmp + " 2>&1";
    FILE* pp = popen(cmd.c_str(), "r"); std::string out; char buf[256];
    while (pp && fgets(buf, sizeof buf, pp)) out += buf;
    const int rc = pp ? pclose(pp) : -1;
    check(rc == 0, "rdna-alu: spirv-val accepts the mixed module");
    if (rc != 0) std::printf("    spirv-val said: %s\n", out.c_str());
    std::remove(tmp.c_str());
  }
}
