// PNR PS-style lit + textured cube demo "game" (developer-controlled), GPL-2.0.
// The game builds a 24-vertex cube (per-face normals, tint colours, uv 0..1 per
// face), an 8x8 texture, and a light; uploads them with pnrGfxSetLitTexturedMesh
// / pnrGfxSetTexture / pnrGfxSetLight (PNR extension calls, NOT Sony APIs) and
// spins it. Frames 0..3: nearest filter; 4..7: linear filter and a new light
// direction. Each frame is presented to /frames/littex_NN.ppm. Offscreen only.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

extern "C" {
const char* pnrGfxLastError();
std::int32_t pnrGfxInit(std::uint32_t, std::uint32_t);
std::int32_t pnrGfxSetTexture(const std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t);
std::int32_t pnrGfxSetLight(float, float, float);
std::int32_t pnrGfxSetLitTexturedMesh(const float*, std::uint32_t, const std::uint32_t*, std::uint32_t);
std::int32_t pnrGfxDrawLitTexturedMesh(float yaw, float pitch);
std::uint32_t pnrGfxLitTexturedDraws();
std::int64_t pnrGfxPresentToFile(const char*);
}

static int fail(const char* what) { std::printf("ps_littex_demo: FAIL %s: %s\n", what, pnrGfxLastError()); return 1; }

int main() {
  constexpr std::uint32_t W = 96, H = 96, N = 8;
  if (pnrGfxInit(W, H) != 0) { std::printf("ps_littex_demo: BLOCKED graphics init: %s\n", pnrGfxLastError()); return 3; }
  struct Face { float n[3], u[3], w[3], c[3]; };
  const Face faces[6] = {
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {1, 0.6f, 0.6f}},
      {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0.6f, 1, 0.6f}},
      {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {0.6f, 0.6f, 1}},
      {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {1, 1, 0.6f}},
      {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0.6f, 1}},
      {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0.6f, 1, 1}}};
  std::vector<float> v;
  std::vector<std::uint32_t> idx;
  for (int f = 0; f < 6; ++f) {
    const float su[4] = {-1, 1, 1, -1}, sw[4] = {-1, -1, 1, 1};
    const std::uint32_t base = static_cast<std::uint32_t>(v.size() / 11);
    for (int i = 0; i < 4; ++i) {
      for (int k = 0; k < 3; ++k) v.push_back(0.5f * (faces[f].n[k] + su[i] * faces[f].u[k] + sw[i] * faces[f].w[k]));
      for (int k = 0; k < 3; ++k) v.push_back(faces[f].c[k]);
      v.push_back((su[i] + 1) * 0.5f);
      v.push_back((sw[i] + 1) * 0.5f);
      for (int k = 0; k < 3; ++k) v.push_back(faces[f].n[k]);
    }
    for (std::uint32_t t : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + t);
  }
  std::vector<std::uint8_t> tex(8 * 8 * 4);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      std::uint8_t* p = &tex[(y * 8 + x) * 4];
      p[0] = std::uint8_t(32 * x + 16); p[1] = std::uint8_t(32 * y + 16);
      p[2] = ((x + y) & 1) ? 255 : 64; p[3] = 255;
    }
  if (pnrGfxDrawLitTexturedMesh(0, 0) != -7) return fail("draw before mesh must return -7");
  std::vector<float> badn = v; badn[8] = 3.0f;
  std::vector<float> badf = v; badf[9] = std::numeric_limits<float>::infinity();
  std::vector<std::uint32_t> badi = idx; badi[7] = 24;
  int refused = 0;
  refused += pnrGfxSetLitTexturedMesh(nullptr, 24, idx.data(), 36) == -6;
  refused += pnrGfxSetLitTexturedMesh(v.data(), 65, idx.data(), 36) == -6;
  refused += pnrGfxSetLitTexturedMesh(v.data(), 24, idx.data(), 35) == -6;
  refused += pnrGfxSetLitTexturedMesh(badn.data(), 24, idx.data(), 36) == -6;
  refused += pnrGfxSetLitTexturedMesh(badf.data(), 24, idx.data(), 36) == -6;
  refused += pnrGfxSetLitTexturedMesh(v.data(), 24, badi.data(), 36) == -6;
  if (refused != 6) { std::printf("ps_littex_demo: only %d of 6 invalid calls refused\n", refused); return fail("validation"); }
  if (pnrGfxDrawLitTexturedMesh(0, 0) != -7) return fail("rejected uploads must not leave a mesh behind");
  if (pnrGfxSetLitTexturedMesh(v.data(), 24, idx.data(), 36) != 0) return fail("pnrGfxSetLitTexturedMesh");
  if (pnrGfxDrawLitTexturedMesh(0, 0) != -8) return fail("draw before texture must return -8");
  std::printf("ps_littex_demo: 24-vertex lit textured cube uploaded; 6 invalid calls refused\n");
  for (std::uint32_t k = 0; k < N; ++k) {
    if (k == 0) {
      if (pnrGfxSetTexture(tex.data(), 8, 8, 0) != 0) return fail("texture nearest");
      if (pnrGfxSetLight(0.4f, 0.6f, 0.7f) != 0) return fail("light");
    }
    if (k == 4) {
      if (pnrGfxSetTexture(tex.data(), 8, 8, 1) != 0) return fail("texture linear");
      if (pnrGfxSetLight(-0.6f, 0.3f, 0.7f) != 0) return fail("light");
    }
    const float yaw = k * 2.0f * static_cast<float>(M_PI) / N + 0.3f;
    const std::int32_t cov = pnrGfxDrawLitTexturedMesh(yaw, 0.5f);
    if (cov <= 0) return fail("draw lit textured");
    char p[64];
    std::snprintf(p, sizeof p, "/frames/littex_%02u.ppm", k);
    if (pnrGfxPresentToFile(p) <= 0) return fail("present");
    std::printf("ps_littex_demo: frame %u %s yaw=%.3f covered=%d\n", k, k < 4 ? "nearest" : "linear", yaw, cov);
  }
  if (pnrGfxLitTexturedDraws() != N) return fail("draw counter");
  std::printf("ps_littex_demo: ALL FRAMES RENDERED AND PRESENTED TO JAIL\n");
  return 0;
}
