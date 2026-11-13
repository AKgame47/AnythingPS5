// PNR PS-style graphics demo "game" (developer-controlled), GPL-2.0.
// Its frame loop runs inside the game process under `pnr run`: each frame it
// advances the rotation, asks the PNR shim for an offscreen 3D render
// (pnrGfx* - PNR extension calls, NOT Sony APIs), "presents" the frame to a
// PPM file in its jail, then verifies the files it wrote through sceKernel*
// file calls. The presented frames use the translated-RDNA vertex path
// (pnrGfxDrawCubeRdna); the hand-authored vertex shader is rendered too as a
// reference and the two PPM files must be byte-identical. There is no
// swapchain and no display.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
std::int32_t sceKernelUsleep(std::uint32_t);
std::int64_t sceKernelOpen(const char*, std::int64_t);
std::int64_t sceKernelRead(std::int64_t, void*, std::size_t);
std::int32_t sceKernelClose(std::int64_t);
std::int32_t sceKernelFstat(std::int64_t, std::int64_t*);
const char* pnrGfxLastError();
std::int32_t pnrGfxInit(std::uint32_t, std::uint32_t);
std::int32_t pnrGfxDrawCube(float yaw, float pitch);
std::int32_t pnrGfxDrawCubeRdna(float yaw, float pitch);
std::uint32_t pnrGfxRdnaFramesRendered();
std::int64_t pnrGfxPresentToFile(const char*);
std::uint32_t pnrGfxFramesRendered();
}

static int fail(const char* what) {
  std::printf("ps_cube_demo: FAIL %s\n", what);
  return 1;
}

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
  if (pnrGfxInit(W, H) != 0) {
    std::printf("ps_cube_demo: BLOCKED graphics init: %s\n", pnrGfxLastError());
    return 3;
  }
  std::printf("ps_cube_demo: offscreen renderer up (%ux%u, RDNA-translated vertex stage, no swapchain/display)\n", W, H);
  double worst_ms = 0, total_ms = 0;
  std::int32_t cov[N];
  for (std::uint32_t k = 0; k < N; ++k) {
    const auto t0 = std::chrono::steady_clock::now();
    const float yaw = k * 2.0f * static_cast<float>(M_PI) / N;
    char rpath[64];
    std::snprintf(rpath, sizeof rpath, "/ref/cube_%02u.ppm", k);
    const std::int32_t ref_cov = pnrGfxDrawCube(yaw, 0.5f);  // hand-authored VS reference
    if (ref_cov <= 0) { std::printf("ps_cube_demo: draw error: %s\n", pnrGfxLastError()); return fail("pnrGfxDrawCube"); }
    if (pnrGfxPresentToFile(rpath) <= 0) return fail("present reference");
    cov[k] = pnrGfxDrawCubeRdna(yaw, 0.5f);  // frame actually presented
    if (cov[k] <= 0) { std::printf("ps_cube_demo: draw error: %s\n", pnrGfxLastError()); return fail("pnrGfxDrawCubeRdna"); }
    char path[64];
    std::snprintf(path, sizeof path, "/frames/cube_%02u.ppm", k);
    if (pnrGfxPresentToFile(path) <= 0) {
      std::printf("ps_cube_demo: present error: %s\n", pnrGfxLastError());
      return fail("pnrGfxPresentToFile");
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    total_ms += ms;
    if (ms > worst_ms) worst_ms = ms;
    std::printf("ps_cube_demo: frame %u yaw=%.3f covered=%d frame_ms=%.1f\n", k, yaw, cov[k], ms);
    sceKernelUsleep(16000);  // pace the loop toward ~60 Hz; real frame time includes the render
  }
  if (pnrGfxRdnaFramesRendered() != N || pnrGfxFramesRendered() != 2 * N) return fail("frame counters");
  // verify what was written, via the jailed file calls
  const std::size_t expect = std::strlen("P6\n96 96\n255\n") + W * H * 3;
  std::vector<unsigned char> f0, f2;
  for (std::uint32_t k = 0; k < N; ++k) {
    char path[64];
    std::snprintf(path, sizeof path, "/frames/cube_%02u.ppm", k);
    std::vector<unsigned char> d;
    if (!read_file(path, &d)) return fail("read frame back");
    if (d.size() != expect || std::memcmp(d.data(), "P6\n96 96\n255\n", 13) != 0) return fail("frame file shape");
    char rp[64];
    std::snprintf(rp, sizeof rp, "/ref/cube_%02u.ppm", k);
    std::vector<unsigned char> r;
    if (!read_file(rp, &r)) return fail("read reference frame back");
    if (r != d) { std::printf("ps_cube_demo: frame %u differs from hand-authored reference\n", k); return fail("RDNA frame != reference"); }
    if (k == 0) f0 = d;
    if (k == 2) f2 = d;
  }
  if (f0 == f2) return fail("frames 0 and 2 identical (no animation)");
  std::printf("ps_cube_demo: %u frames, avg %.1f ms, worst %.1f ms (software Vulkan)\n", N, total_ms / N, worst_ms);
  std::printf("ps_cube_demo: %u RDNA-vertex frames byte-identical to hand-authored reference\n", N);
  std::printf("ps_cube_demo: ALL FRAMES RENDERED, PRESENTED TO JAIL AND VERIFIED\n");
  return 0;
}
