// PNR window demo "game" (developer-controlled), GPL-2.0. Slice S3.
// Opens a real SDL2 + Vulkan window and presents a spinning cube whose clip
// positions come from the translated-RDNA fused multiply-add path (slice S2)
// every frame, rendered through the clip-space path and presented through a
// real swapchain. This is PNR's own demo content, NOT PS5 game output.
// Options: --frames N (default 240), --width W --height H, --shot F:path.ppm
// (save frame F's host image), --delay MS (extra per-frame sleep, default 0).
// Exit: 0 presented all frames; 3 BLOCKED (no display/Vulkan/present queue);
// 1 a real failure.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <SDL.h>

#include "graphics/demo/clip_shaders.hpp"
#include "graphics/demo/spin_shaders.hpp"
#include "graphics/mat4.hpp"
#include "graphics/rdna_vertex.hpp"
#include "graphics/window.hpp"

using namespace pnr::graphics;

static void save_ppm(const char* path, const std::vector<uint8_t>& rgba,
                     uint32_t w, uint32_t h) {
  std::ofstream o(path, std::ios::binary);
  o << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i + 2 < rgba.size(); i += 4)
    o.write(reinterpret_cast<const char*>(&rgba[i]), 3);
}

int main(int argc, char** argv) {
  uint32_t frames = 240, w = 640, h = 480, delay_ms = 0;
  int shot_frame = -1;
  std::string shot_path;
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "pnr-window-demo: %s needs a value\n", name); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--frames")) frames = (uint32_t)std::atoi(need("--frames"));
    else if (!std::strcmp(argv[i], "--width")) w = (uint32_t)std::atoi(need("--width"));
    else if (!std::strcmp(argv[i], "--height")) h = (uint32_t)std::atoi(need("--height"));
    else if (!std::strcmp(argv[i], "--delay")) delay_ms = (uint32_t)std::atoi(need("--delay"));
    else if (!std::strcmp(argv[i], "--shot")) {
      const char* v = need("--shot");
      const char* colon = std::strchr(v, ':');
      if (!colon || colon == v) { std::fprintf(stderr, "pnr-window-demo: --shot is F:path\n"); return 2; }
      shot_frame = std::atoi(v);
      shot_path = colon + 1;
    } else { std::fprintf(stderr, "pnr-window-demo: unknown option %s\n", argv[i]); return 2; }
  }
  if (!frames || frames > 100000 || !w || !h || w > 4096 || h > 4096) {
    std::fprintf(stderr, "pnr-window-demo: bad frames/size\n");
    return 2;
  }

  VulkanBackend vk;
  auto st = vk.open();
  if (!st) { std::fprintf(stderr, "pnr-window-demo: BLOCKED %s\n", st.error.message.c_str()); return 3; }
  std::unique_ptr<WindowPresenter> win;
  WindowConfig cfg;
  cfg.width = w; cfg.height = h;
  cfg.title = "PNR window demo - RDNA fma cube";
  cfg.app_name = "pnr-window-demo";
  st = WindowPresenter::create(vk, cfg, &win);
  if (!st) { std::fprintf(stderr, "pnr-window-demo: BLOCKED %s\n", st.error.message.c_str()); return 3; }
  std::printf("pnr-window-demo: window %ux%u on %s, swapchain %s\n", w, h,
              vk.device_name().c_str(), win->swapchain_bgra() ? "B8G8R8A8" : "R8G8B8A8");

  std::vector<float> verts;
  std::vector<uint32_t> idx;
  cube_mesh(&verts, &idx);
  std::vector<float> xyz, col;
  for (size_t i = 0; i < verts.size(); i += 6) {
    for (int k = 0; k < 3; ++k) { xyz.push_back(verts[i + k]); col.push_back(verts[i + 3 + k]); }
  }
  std::vector<uint32_t> cvs(std::begin(clip_vert), std::end(clip_vert));
  std::vector<uint32_t> fs(std::begin(spin_frag), std::end(spin_frag));
  const float black[4] = {0, 0, 0, 1};
  const Mat4 P = mat4_perspective(1.0471976f, float(w) / float(h), 0.1f, 10.0f);
  const Mat4 T = mat4_translate(0, 0, -3);

  uint32_t done = 0;
  for (uint32_t k = 0; k < frames; ++k) {
    if (!win->pump()) { std::printf("pnr-window-demo: window closed after %u frames\n", done); break; }
    const float angle = float(k) * 0.03f;
    const Mat4 mvp = mat4_mul(P, mat4_mul(T, mat4_mul(mat4_rotate_x(0.5f), mat4_rotate_y(angle))));
    // Vertex stage on translated RDNA machine code (fused multiply-add path).
    std::vector<float> clip;
    st = rdna_transform_points(vk, xyz, mat4_words(mvp), &clip, true);
    if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL transform: %s\n", st.error.message.c_str()); return 1; }
    std::vector<float> cv;
    for (size_t i = 0; i < 24; ++i) {
      for (int c = 0; c < 4; ++c) cv.push_back(clip[i * 4 + c]);
      for (int c = 0; c < 3; ++c) cv.push_back(col[i * 3 + c]);
    }
    std::vector<uint8_t> img;
    st = vk.render_clip_mesh_rgba(w, h, cvs, fs, cv, idx, true, black, &img);
    if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL render: %s\n", st.error.message.c_str()); return 1; }
    st = win->present(img, w, h);
    if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL present: %s\n", st.error.message.c_str()); return 1; }
    if ((int)k == shot_frame) { save_ppm(shot_path.c_str(), img, w, h); std::printf("pnr-window-demo: saved %s\n", shot_path.c_str()); }
    ++done;
    if (delay_ms) {
      const uint64_t until = SDL_GetTicks() + delay_ms;
      while (SDL_GetTicks() < until) {}
    }
  }
  if (shot_frame >= (int)done && shot_frame >= 0) {
    std::fprintf(stderr, "pnr-window-demo: FAIL shot frame %d was never reached\n", shot_frame);
    return 1;
  }
  std::printf("pnr-window-demo: presented %u frames (RDNA fma vertex path, real swapchain)\n", done);
  return done ? 0 : 1;
}
