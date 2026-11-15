// PNR window demo "game" (developer-controlled), GPL-2.0. Slice S3.
// Opens a real SDL2 + Vulkan window and presents a spinning cube whose clip
// positions come from the translated-RDNA fused multiply-add path (slice S2)
// with the perspective divide also done by translated RDNA code (slice S6)
// every frame, rendered through the clip-space path and presented through a
// real swapchain. This is PNR's own demo content, NOT PS5 game output.
// Options: --frames N (default 240), --width W --height H, --shot F:path.ppm
// (save frame F's host image), --delay MS (extra per-frame sleep, default 0),
// --virtual-pad (attach an SDL virtual joystick and drive a scripted input
// pattern: right stick at frame 2, left at 5, south button down/up at 8/10,
// stick back to neutral at 12; the demo verifies those events arrived and
// prints pad-check at the end).
// Pad input (slice S4): left stick X scales the spin (1+lx), holding the
// south button pauses it. With no pad connected the demo behaves exactly as
// in slice S3.
// Exit: 0 presented all frames; 3 BLOCKED (no display/Vulkan/present queue);
// 1 a real failure.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
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
#include "input/sdl_pad.hpp"
#include "services/input.hpp"

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
  bool virtual_pad = false;
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
    else if (!std::strcmp(argv[i], "--virtual-pad")) virtual_pad = true;
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

  // Pad input (slice S4). --virtual-pad attaches SDL's virtual joystick so
  // hosts without hardware still run the REAL SDL joystick path; then the
  // first joystick (virtual or physical) is opened as the pad source.
  std::unique_ptr<pnr::input::SdlVirtualPad> vpad;
  if (virtual_pad) {
    st = pnr::input::SdlVirtualPad::attach(6, 10, 1, &vpad);
    if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL virtual pad: %s\n", st.error.message.c_str()); return 1; }
    std::printf("pad: attached SDL virtual joystick (6 axes, 10 buttons, 1 hat; no hardware)\n");
  }
  std::unique_ptr<pnr::input::SdlPadSource> pad;
  st = pnr::input::SdlPadSource::open_first(&pad);
  if (st) {
    std::printf("pad: connected '%s' (%d axes, %d buttons, %d hats) - left stick steers spin, holding south button pauses\n",
                pad->device_name().c_str(), pad->axes(), pad->buttons(), pad->hats());
  } else if (virtual_pad) {
    std::fprintf(stderr, "pnr-window-demo: FAIL virtual pad attached but no joystick opens: %s\n", st.error.message.c_str());
    return 1;
  } else {
    std::printf("pad: none connected (left stick would steer spin, south button pause)\n");
  }
  float pad_lx = 0.0f, last_reported_lx = 0.0f;
  bool pad_paused = false;  // south button holds the pause (down = paused, up = resumed)
  size_t pad_axis_events = 0, pad_downs = 0, pad_ups = 0;

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
  float angle = 0.0f;
  for (uint32_t k = 0; k < frames; ++k) {
    if (vpad) {
      // Scripted input pattern (deterministic): the values travel SDL's real
      // joystick state path and are read back by the poll below.
      if (k == 2) st = vpad->set_axis(0, 16384);
      else if (k == 5) st = vpad->set_axis(0, -32768);
      else if (k == 8) st = vpad->set_button(0, true);
      else if (k == 10) st = vpad->set_button(0, false);
      else if (k == 12) st = vpad->set_axis(0, 0);
      if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL virtual pad drive: %s\n", st.error.message.c_str()); return 1; }
    }
    if (!win->pump()) { std::printf("pnr-window-demo: window closed after %u frames\n", done); break; }
    if (pad) {
      std::vector<pnr::services::InputEvent> evs;
      pad->poll(&evs);
      for (const auto& e : evs) {
        if (e.control == "ABS_X") {
          pad_lx = pnr::services::normalize_axis(e.value, 32767, 0.15f);
          ++pad_axis_events;
          if (std::fabs(pad_lx - last_reported_lx) >= 0.05f) {
            std::printf("pad: ABS_X %+.2f -> spin scale %.2f\n", pad_lx, 1.0f + pad_lx);
            last_reported_lx = pad_lx;
          }
        } else if (e.control == "BTN_SOUTH") {
          if (e.value) { pad_paused = true; ++pad_downs; std::printf("pad: BTN_SOUTH down (spin paused while held)\n"); }
          else { pad_paused = false; ++pad_ups; std::printf("pad: BTN_SOUTH up (spin resumed)\n"); }
        }
      }
    }
    angle += 0.03f * (pad_paused ? 0.0f : (1.0f + pad_lx));
    const Mat4 mvp = mat4_mul(P, mat4_mul(T, mat4_mul(mat4_rotate_x(0.5f), mat4_rotate_y(angle))));
    // Vertex stage on translated RDNA machine code (fused multiply-add path),
    // then the slice S6 post-transform: the perspective divide runs in
    // translated RDNA too, and the rasterizer gets ndc with w = 1.
    std::vector<float> clip;
    st = rdna_transform_points(vk, xyz, mat4_words(mvp), &clip, true);
    if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL transform: %s\n", st.error.message.c_str()); return 1; }
    const float vpf[8] = {float(w) / 2, float(w) / 2, float(h) / 2, float(h) / 2, 1.0f, 0.0f, 0.0f, 1.0f};
    std::vector<uint32_t> vp8(8);
    for (int c = 0; c < 8; ++c) std::memcpy(&vp8[c], &vpf[c], 4);
    std::vector<float> ndc, winco;
    st = rdna_post_transform_clip(vk, clip, vp8, &ndc, &winco);
    if (!st) { std::fprintf(stderr, "pnr-window-demo: FAIL post-transform: %s\n", st.error.message.c_str()); return 1; }
    std::vector<float> cv;
    for (size_t i = 0; i < 24; ++i) {
      for (int c = 0; c < 3; ++c) cv.push_back(ndc[i * 3 + c]);
      cv.push_back(1.0f);
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
  std::printf("pnr-window-demo: presented %u frames (RDNA fma vertex + rcp/fma post-transform path, real swapchain)\n", done);
  if (virtual_pad) {
    // The scripted pattern must have arrived through the real SDL joystick
    // path: three axis settings, one button down, one button up.
    if (pad_axis_events >= 3 && pad_downs >= 1 && pad_ups >= 1) {
      std::printf("pad-check: ok axis=%zu button-down=%zu button-up=%zu presented=%u\n",
                  pad_axis_events, pad_downs, pad_ups, done);
    } else {
      std::fprintf(stderr, "pnr-window-demo: FAIL pad-check: axis=%zu down=%zu up=%zu (expected 3/1/1)\n",
                   pad_axis_events, pad_downs, pad_ups);
      return 1;
    }
  }
  return done ? 0 : 1;
}
