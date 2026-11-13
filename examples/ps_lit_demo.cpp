// PNR PS-style lit-mesh demo "game" (developer-controlled), GPL-2.0.
// The game builds its own 24-vertex cube with per-vertex face normals, uploads
// it with pnrGfxSetLitMesh, sets a directional light with pnrGfxSetLight
// (PNR extension calls, NOT Sony APIs), then spins it. Every frame is drawn
// with the hand-authored lit vertex shader (reference) and with the
// translated-RDNA dot3 + transform kernels, presented to its jail and
// compared within a small rounding tolerance. The light changes direction
// half way through. Offscreen only: no swapchain, no display.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
std::int64_t sceKernelOpen(const char*, std::int64_t);
std::int64_t sceKernelRead(std::int64_t, void*, std::size_t);
std::int32_t sceKernelClose(std::int64_t);
std::int32_t sceKernelFstat(std::int64_t, std::int64_t*);
const char* pnrGfxLastError();
std::int32_t pnrGfxInit(std::uint32_t, std::uint32_t);
std::int32_t pnrGfxSetLitMesh(const float*, std::uint32_t, const std::uint32_t*, std::uint32_t);
std::int32_t pnrGfxSetLight(float, float, float);
std::int32_t pnrGfxDrawLitMesh(float yaw, float pitch, std::int32_t mode);
std::uint32_t pnrGfxLitDraws();
std::int64_t pnrGfxPresentToFile(const char*);
}

static int fail(const char* what) { std::printf("ps_lit_demo: FAIL %s: %s\n", what, pnrGfxLastError()); return 1; }

static bool read_file(const char* path, std::vector<unsigned char>* out) {
  const std::int64_t fd = sceKernelOpen(path, 0x0);
  if (fd < 0) return false;
  std::int64_t size = 0;
  if (sceKernelFstat(fd, &size) != 0 || size <= 0) { sceKernelClose(fd); return false; }
  out->resize(static_cast<std::size_t>(size));
  const bool ok = sceKernelRead(fd, out->data(), out->size()) == size;
  sceKernelClose(fd);
  return ok;
}

int main() {
  constexpr std::uint32_t W = 96, H = 96, N = 8;
  if (pnrGfxInit(W, H) != 0) { std::printf("ps_lit_demo: BLOCKED graphics init: %s\n", pnrGfxLastError()); return 3; }
  // Cube with flat per-face normals: x y z r g b nx ny nz
  struct Face { float n[3], u[3], w[3], c[3]; };
  const Face faces[6] = {
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {1, 0.3f, 0.3f}},
      {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0.3f, 1, 0.3f}},
      {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {0.3f, 0.3f, 1}},
      {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {1, 1, 0.3f}},
      {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0.3f, 1}},
      {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0.3f, 1, 1}}};
  std::vector<float> v;
  std::vector<std::uint32_t> idx;
  for (int f = 0; f < 6; ++f) {
    const float su[4] = {-1, 1, 1, -1}, sw[4] = {-1, -1, 1, 1};
    const std::uint32_t base = static_cast<std::uint32_t>(v.size() / 9);
    for (int i = 0; i < 4; ++i) {
      for (int k = 0; k < 3; ++k) v.push_back(0.5f * (faces[f].n[k] + su[i] * faces[f].u[k] + sw[i] * faces[f].w[k]));
      for (int k = 0; k < 3; ++k) v.push_back(faces[f].c[k]);
      for (int k = 0; k < 3; ++k) v.push_back(faces[f].n[k]);
    }
    for (std::uint32_t t : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + t);
  }
  if (pnrGfxDrawLitMesh(0, 0, 0) != -7) return fail("draw before upload must return -7");
  // invalid uploads / lights refused
  std::vector<float> badn = v;
  badn[6] = 3.0f;  // first normal no longer unit length
  std::vector<float> badf = v;
  badf[2] = std::numeric_limits<float>::infinity();
  std::vector<std::uint32_t> badi = idx;
  badi[7] = 24;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  int refused = 0;
  refused += pnrGfxSetLitMesh(nullptr, 24, idx.data(), 36) == -6;
  refused += pnrGfxSetLitMesh(v.data(), 25, idx.data(), 36) == -6;
  refused += pnrGfxSetLitMesh(v.data(), 24, idx.data(), 35) == -6;
  refused += pnrGfxSetLitMesh(badn.data(), 24, idx.data(), 36) == -6;
  refused += pnrGfxSetLitMesh(badf.data(), 24, idx.data(), 36) == -6;
  refused += pnrGfxSetLitMesh(v.data(), 24, badi.data(), 36) == -6;
  refused += pnrGfxSetLight(0, 0, 0) == -6;
  refused += pnrGfxSetLight(nan, 1, 1) == -6;
  if (refused != 8) { std::printf("ps_lit_demo: only %d of 8 invalid calls refused\n", refused); return fail("validation"); }
  if (pnrGfxDrawLitMesh(0, 0, 0) != -7) return fail("rejected uploads must not leave a mesh behind");
  if (pnrGfxSetLitMesh(v.data(), 24, idx.data(), 36) != 0) return fail("pnrGfxSetLitMesh");
  std::printf("ps_lit_demo: uploaded 24-vertex lit cube with face normals; 8 invalid calls refused\n");
  std::size_t total_diff = 0;
  std::int32_t cov[N];
  for (std::uint32_t k = 0; k < N; ++k) {
    if (k == 0 && pnrGfxSetLight(0.4f, 0.6f, 0.7f) != 0) return fail("light");
    if (k == 4 && pnrGfxSetLight(-0.6f, 0.3f, 0.7f) != 0) return fail("light");
    const float yaw = k * 2.0f * static_cast<float>(M_PI) / N + 0.3f;
    const std::int32_t ref = pnrGfxDrawLitMesh(yaw, 0.5f, 0);
    if (ref <= 0) return fail("draw hand-authored lit");
    char rp[64], p[64];
    std::snprintf(rp, sizeof rp, "/ref/lit_%02u.ppm", k);
    std::snprintf(p, sizeof p, "/frames/lit_%02u.ppm", k);
    if (pnrGfxPresentToFile(rp) <= 0) return fail("present reference");
    cov[k] = pnrGfxDrawLitMesh(yaw, 0.5f, 1);
    if (cov[k] <= 0) return fail("draw RDNA lit");
    if (pnrGfxPresentToFile(p) <= 0) return fail("present");
    std::printf("ps_lit_demo: frame %u yaw=%.3f covered=%d (reference %d)\n", k, yaw, cov[k], ref);
  }
  if (pnrGfxLitDraws() != 2 * N) return fail("lit draw counter");
  const std::size_t expect = std::strlen("P6\n96 96\n255\n") + W * H * 3;
  std::vector<unsigned char> first, second;
  for (std::uint32_t k = 0; k < N; ++k) {
    char rp[64], p[64];
    std::snprintf(rp, sizeof rp, "/ref/lit_%02u.ppm", k);
    std::snprintf(p, sizeof p, "/frames/lit_%02u.ppm", k);
    std::vector<unsigned char> d, r;
    if (!read_file(p, &d) || !read_file(rp, &r)) return fail("read frames back");
    if (d.size() != expect || std::memcmp(d.data(), "P6\n96 96\n255\n", 13) != 0) return fail("frame file shape");
    std::size_t diff = 0;
    int worst = 0;
    for (std::size_t i = 0; i < d.size(); ++i)
      if (d[i] != r[i]) { ++diff; worst = std::max(worst, std::abs(int(d[i]) - int(r[i]))); }
    total_diff += diff;
    if (diff > d.size() / 100 || worst > 3) {
      std::printf("ps_lit_demo: frame %u: %zu bytes differ (max %d)\n", k, diff, worst);
      return 1;
    }
    if (k == 0) first = d;
    if (k == 1) second = d;
  }
  if (first == second) return fail("frames 0 and 1 identical (no animation)");
  std::printf("ps_lit_demo: %u lit frames, RDNA path vs reference: %zu differing bytes (tolerance +-3 on 1%%)\n", N, total_diff);
  std::printf("ps_lit_demo: ALL FRAMES RENDERED, PRESENTED TO JAIL AND VERIFIED\n");
  return 0;
}
