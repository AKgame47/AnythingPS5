// PNR PS-style mesh demo "game" (developer-controlled), GPL-2.0.
// The game supplies its OWN vertices and indices (a 5-vertex pyramid) through
// pnrGfxSetMesh (PNR extension, NOT a Sony API), exercises the validation
// paths, then spins the mesh. Each frame is drawn twice: with the hand-authored
// vertex shader (reference) and with the translated-RDNA compute-as-vertex
// path. Both are presented to its jail and compared byte-wise within a tiny
// tolerance (rounding of fused vs unfused float ops). Offscreen
// only: no swapchain, no display.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
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
std::int32_t pnrGfxSetMesh(const float*, std::uint32_t, const std::uint32_t*, std::uint32_t);
std::int32_t pnrGfxDrawMesh(float yaw, float pitch, std::int32_t mode);
std::uint32_t pnrGfxMeshDraws();
std::int64_t pnrGfxPresentToFile(const char*);
}

static int fail(const char* what) { std::printf("ps_mesh_demo: FAIL %s: %s\n", what, pnrGfxLastError()); return 1; }

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
  if (pnrGfxInit(W, H) != 0) { std::printf("ps_mesh_demo: BLOCKED graphics init: %s\n", pnrGfxLastError()); return 3; }
  // x y z r g b
  const float pyramid[5 * 6] = {
      0.0f, 0.6f, 0.0f, 1.0f, 1.0f, 1.0f,    // apex, white
      -0.5f, -0.4f, -0.5f, 1.0f, 0.2f, 0.2f,  // base corners
      0.5f, -0.4f, -0.5f, 0.2f, 1.0f, 0.2f,
      0.5f, -0.4f, 0.5f, 0.2f, 0.2f, 1.0f,
      -0.5f, -0.4f, 0.5f, 1.0f, 1.0f, 0.2f};
  const std::uint32_t idx[18] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1, 1, 3, 2, 1, 4, 3};
  // drawing before any upload is refused
  if (pnrGfxDrawMesh(0, 0, 0) != -7) return fail("draw before upload must return -7");
  // invalid uploads are refused with -6 and keep no state
  const float nan = std::numeric_limits<float>::quiet_NaN();
  float badf[5 * 6];
  std::memcpy(badf, pyramid, sizeof badf);
  badf[7] = nan;
  std::uint32_t badi[18];
  std::memcpy(badi, idx, sizeof badi);
  badi[4] = 5;
  const std::uint32_t big[3] = {0, 1, 2};
  int refused = 0;
  refused += pnrGfxSetMesh(nullptr, 5, idx, 18) == -6;
  refused += pnrGfxSetMesh(pyramid, 5, nullptr, 18) == -6;
  refused += pnrGfxSetMesh(pyramid, 0, idx, 18) == -6;
  refused += pnrGfxSetMesh(pyramid, 25, idx, 18) == -6;
  refused += pnrGfxSetMesh(pyramid, 5, idx, 17) == -6;
  refused += pnrGfxSetMesh(pyramid, 5, idx, 0) == -6;
  refused += pnrGfxSetMesh(badf, 5, idx, 18) == -6;
  refused += pnrGfxSetMesh(pyramid, 5, badi, 18) == -6;
  refused += pnrGfxSetMesh(pyramid, 2, big, 3) == -6;  // index 2 >= 2 vertices
  if (refused != 9) { std::printf("ps_mesh_demo: only %d of 9 invalid uploads refused\n", refused); return fail("validation"); }
  if (pnrGfxDrawMesh(0, 0, 0) != -7) return fail("rejected uploads must not leave a mesh behind");
  if (pnrGfxSetMesh(pyramid, 5, idx, 18) != 0) return fail("pnrGfxSetMesh");
  if (pnrGfxDrawMesh(0, 0, 2) != -6) return fail("bad draw mode must return -6");
  std::printf("ps_mesh_demo: uploaded 5-vertex 6-triangle pyramid; 9 invalid uploads refused\n");
  std::int32_t cov[N];
  for (std::uint32_t k = 0; k < N; ++k) {
    const float yaw = k * 2.0f * static_cast<float>(M_PI) / N;
    const std::int32_t ref = pnrGfxDrawMesh(yaw, 0.4f, 0);
    if (ref <= 0) return fail("draw hand-authored");
    char rp[64], p[64];
    std::snprintf(rp, sizeof rp, "/ref/mesh_%02u.ppm", k);
    std::snprintf(p, sizeof p, "/frames/mesh_%02u.ppm", k);
    if (pnrGfxPresentToFile(rp) <= 0) return fail("present reference");
    cov[k] = pnrGfxDrawMesh(yaw, 0.4f, 1);
    if (cov[k] <= 0) return fail("draw RDNA path");
    if (pnrGfxPresentToFile(p) <= 0) return fail("present");
    std::printf("ps_mesh_demo: frame %u yaw=%.3f covered=%d (reference %d)\n", k, yaw, cov[k], ref);
  }
  if (pnrGfxMeshDraws() != 2 * N) return fail("mesh draw counter");
  const std::size_t expect = std::strlen("P6\n96 96\n255\n") + W * H * 3;
  std::vector<unsigned char> first, third;
  std::size_t total_diff = 0;
  for (std::uint32_t k = 0; k < N; ++k) {
    char rp[64], p[64];
    std::snprintf(rp, sizeof rp, "/ref/mesh_%02u.ppm", k);
    std::snprintf(p, sizeof p, "/frames/mesh_%02u.ppm", k);
    std::vector<unsigned char> d, r;
    if (!read_file(p, &d) || !read_file(rp, &r)) return fail("read frames back");
    if (d.size() != expect || std::memcmp(d.data(), "P6\n96 96\n255\n", 13) != 0) return fail("frame file shape");
    // The RDNA kernel uses unfused binary32 mul/add; the hand-authored vertex
    // shader may fuse. Allow at most +-2 per channel on at most 0.5% of the
    // bytes (the typical measured result is exactly equal) and report it.
    std::size_t diff = 0;
    int worst = 0;
    for (std::size_t i = 0; i < d.size(); ++i)
      if (d[i] != r[i]) { ++diff; worst = std::max(worst, std::abs(int(d[i]) - int(r[i]))); }
    total_diff += diff;
    if (diff > d.size() / 200 || worst > 2) {
      std::printf("ps_mesh_demo: frame %u: %zu bytes differ (max %d)\n", k, diff, worst);
      return 1;
    }
    if (k == 0) first = d;
    if (k == 2) third = d;
  }
  if (first == third) return fail("frames 0 and 2 identical (no animation)");
  std::printf("ps_mesh_demo: %u frames of the game's own mesh, RDNA path vs reference: %zu differing bytes (tolerance +-2 on 0.5%%)\n", N, total_diff);
  std::printf("ps_mesh_demo: ALL FRAMES RENDERED, PRESENTED TO JAIL AND VERIFIED\n");
  return 0;
}
