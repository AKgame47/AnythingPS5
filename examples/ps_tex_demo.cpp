// PNR PS-style textured-quad demo "game" (developer-controlled), GPL-2.0.
// The game builds an 8x8 RGBA texture and a quad with uv coordinates, uploads
// both through pnrGfxSetTexture / pnrGfxSetTexturedMesh (PNR extension calls,
// NOT Sony APIs) and spins the quad. Frames 0..3 use nearest + clamp with uv
// 0..1; frames 4..7 re-upload with linear + repeat and uv 0..2. Each frame is
// presented to /frames/tex_NN.ppm. Offscreen only: no swapchain, no display.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

extern "C" {
const char* pnrGfxLastError();
std::int32_t pnrGfxInit(std::uint32_t, std::uint32_t);
std::int32_t pnrGfxSetTexture(const std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t);
std::int32_t pnrGfxSetTexturedMesh(const float*, std::uint32_t, const std::uint32_t*, std::uint32_t);
std::int32_t pnrGfxDrawTexturedMesh(float yaw, float pitch);
std::uint32_t pnrGfxTexturedDraws();
std::int64_t pnrGfxPresentToFile(const char*);
}

static int fail(const char* what) { std::printf("ps_tex_demo: FAIL %s: %s\n", what, pnrGfxLastError()); return 1; }

static std::vector<float> quad(float uvscale) {
  // x y z r g b u v; corners (-.7,.7) top-left .. (.7,-.7) bottom-right
  const float c[4][2] = {{-0.7f, 0.7f}, {0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
  const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  std::vector<float> v;
  for (int i = 0; i < 4; ++i) {
    v.insert(v.end(), {c[i][0], c[i][1], 0.0f, 1.0f, 1.0f, 1.0f, uv[i][0] * uvscale, uv[i][1] * uvscale});
  }
  return v;
}

int main() {
  constexpr std::uint32_t W = 96, H = 96, N = 8;
  if (pnrGfxInit(W, H) != 0) { std::printf("ps_tex_demo: BLOCKED graphics init: %s\n", pnrGfxLastError()); return 3; }
  std::vector<std::uint8_t> tex(8 * 8 * 4);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      std::uint8_t* p = &tex[(y * 8 + x) * 4];
      p[0] = std::uint8_t(32 * x + 16); p[1] = std::uint8_t(32 * y + 16);
      p[2] = ((x + y) & 1) ? 255 : 64; p[3] = 255;
    }
  const std::vector<std::uint32_t> idx = {0, 1, 2, 0, 2, 3};
  std::vector<float> q1 = quad(1.0f), q2 = quad(2.0f);
  if (pnrGfxDrawTexturedMesh(0, 0) != -7) return fail("draw before mesh must return -7");
  if (pnrGfxSetTexturedMesh(q1.data(), 4, idx.data(), 6) != 0) return fail("pnrGfxSetTexturedMesh");
  if (pnrGfxDrawTexturedMesh(0, 0) != -8) return fail("draw before texture must return -8");
  std::vector<float> badf = q1;
  badf[7] = std::numeric_limits<float>::quiet_NaN();
  std::vector<std::uint32_t> badi = idx;
  badi[5] = 4;
  int refused = 0;
  refused += pnrGfxSetTexture(nullptr, 8, 8, 0) == -6;
  refused += pnrGfxSetTexture(tex.data(), 0, 8, 0) == -6;
  refused += pnrGfxSetTexture(tex.data(), 8, 257, 0) == -6;
  refused += pnrGfxSetTexture(tex.data(), 8, 8, 4) == -6;
  refused += pnrGfxSetTexturedMesh(nullptr, 4, idx.data(), 6) == -6;
  refused += pnrGfxSetTexturedMesh(q1.data(), 65, idx.data(), 6) == -6;
  refused += pnrGfxSetTexturedMesh(q1.data(), 4, idx.data(), 5) == -6;
  refused += pnrGfxSetTexturedMesh(badf.data(), 4, idx.data(), 6) == -6;
  refused += pnrGfxSetTexturedMesh(q1.data(), 4, badi.data(), 6) == -6;
  if (refused != 9) { std::printf("ps_tex_demo: only %d of 9 invalid calls refused\n", refused); return fail("validation"); }
  if (pnrGfxDrawTexturedMesh(0, 0) != -8) return fail("rejected texture upload must not leave a texture behind");
  std::printf("ps_tex_demo: 8x8 texture + uv quad ready; 9 invalid calls refused\n");
  for (std::uint32_t k = 0; k < N; ++k) {
    if (k == 0 && pnrGfxSetTexture(tex.data(), 8, 8, 0) != 0) return fail("texture nearest/clamp");
    if (k == 4) {
      if (pnrGfxSetTexture(tex.data(), 8, 8, 3) != 0) return fail("texture linear/repeat");
      if (pnrGfxSetTexturedMesh(q2.data(), 4, idx.data(), 6) != 0) return fail("mesh uv 0..2");
    }
    const float yaw = (k % 4) * 0.35f - 0.5f;
    const std::int32_t cov = pnrGfxDrawTexturedMesh(yaw, 0.3f);
    if (cov <= 0) return fail("draw textured");
    char p[64];
    std::snprintf(p, sizeof p, "/frames/tex_%02u.ppm", k);
    if (pnrGfxPresentToFile(p) <= 0) return fail("present");
    std::printf("ps_tex_demo: frame %u %s yaw=%.2f covered=%d\n", k, k < 4 ? "nearest/clamp" : "linear/repeat", yaw, cov);
  }
  if (pnrGfxTexturedDraws() != N) return fail("textured draw counter");
  std::printf("ps_tex_demo: ALL FRAMES RENDERED AND PRESENTED TO JAIL\n");
  return 0;
}
