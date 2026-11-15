// PNR Graphics - draw from tracked GNM state (Core slice C4d), GPL-2.0. See gfx_draw.hpp for the ABI.
#include "graphics/gfx_draw.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "graphics/demo/clip_shaders.hpp"
#include "graphics/demo/spin_shaders.hpp"
#include "graphics/rdna_pixel.hpp"
#include "graphics/rdna_vertex.hpp"
#include "shader/translate.hpp"

namespace pnr::graphics {

std::vector<std::uint32_t> make_pnr_vs_blob(std::uint32_t n) {
  std::vector<std::vector<std::uint32_t>> k;
  for (std::uint32_t c = 0; c < 4; ++c) k.push_back(rdna_vertex_component_kernel(c, n));
  std::vector<std::uint32_t> blob = {kPnrVsMagic, 4};
  for (auto& v : k) blob.push_back(static_cast<std::uint32_t>(v.size()));
  for (auto& v : k) blob.insert(blob.end(), v.begin(), v.end());
  return blob;
}

std::vector<std::uint32_t> make_pnr_vs2_blob(std::uint32_t n) {
  std::vector<std::vector<std::uint32_t>> k;
  for (std::uint32_t c = 0; c < 4; ++c) k.push_back(rdna_vertex_component_kernel(c, n));
  for (std::uint32_t c = 0; c < 3; ++c) k.push_back(rdna_post_ndc_kernel(c, n));
  std::vector<std::uint32_t> blob = {kPnrVs2Magic, 7};
  for (auto& v : k) blob.push_back(static_cast<std::uint32_t>(v.size()));
  for (auto& v : k) blob.insert(blob.end(), v.begin(), v.end());
  return blob;
}

namespace {
DrawResult fail(DrawResult r, const std::string& why) { r.ok = false; r.error = why; return r; }
bool read_u64(const ShaderBinding& s, int slot, std::uint64_t* out) {
  if (!s.user_data_set[slot] || !s.user_data_set[slot + 1]) return false;
  *out = (std::uint64_t(s.user_data[slot + 1]) << 32) | s.user_data[slot];
  return true;
}
}  // namespace

DrawResult execute_draw(VulkanBackend& vk, GpuAddressSpace& mem, const pm4::DrawCall& draw) {
  DrawResult r;
  if (draw.kind != pm4::DrawKind::Auto && draw.kind != pm4::DrawKind::Indexed)
    return fail(r, "only DRAW_INDEX_AUTO and DRAW_INDEX_INDX are supported");
  if (draw.instances != 1) return fail(r, "only one instance is supported");
  const DrawBindings b = extract_bindings(draw);
  if (!b.missing.empty()) return fail(r, "draw state incomplete: " + b.missing);
  if (b.primitive != amd::PrimitiveType::TriangleList) return fail(r, "only triangle lists are supported");
  const ShaderBinding* vs = b.vertex_stage();
  if (!vs) return fail(r, "no vertex-stage shader address set");
  if (!b.ps.present) return fail(r, "no pixel-stage shader address set");
  if (!b.color0.present || !b.has_scissor) return fail(r, "colour target 0 or screen scissor not set");
  // Slice S7: indexed draws fetch the index buffer from guest memory. The
  // vertex transform runs over the referenced vertices (max index + 1, still
  // bound by the 24-lane kernel limit); indices pass through unchanged.
  std::vector<std::uint32_t> indices;
  std::uint32_t n = draw.count;
  if (draw.kind == pm4::DrawKind::Indexed) {
    if (draw.index_size_bytes != 2 && draw.index_size_bytes != 4)
      return fail(r, "index type not set before the draw (16-bit and 32-bit indices are supported)");
    if (draw.count == 0 || draw.count % 3)
      return fail(r, "index count must be a multiple of 3");
    const std::uint64_t ibytes = std::uint64_t(draw.count) * draw.index_size_bytes;
    const std::uint8_t* ip = mem.resolve(draw.index_va, ibytes);
    if (!ip) return fail(r, "index buffer is not mapped: " + mem.last_error());
    indices.resize(draw.count);
    std::uint32_t maxidx = 0;
    for (std::uint32_t i = 0; i < draw.count; ++i) {
      std::uint32_t v = 0;
      if (draw.index_size_bytes == 2) { std::uint16_t t; std::memcpy(&t, ip + 2 * i, 2); v = t; }
      else std::memcpy(&v, ip + 4 * i, 4);
      indices[i] = v;
      if (v > maxidx) maxidx = v;
    }
    n = maxidx + 1;
    if (n > kRdnaVertexMax) return fail(r, "indexed draw references more than 24 vertices (kernel lane limit)");
  } else if (n == 0 || n % 3 || n > kRdnaVertexMax) {
    return fail(r, "vertex count must be a multiple of 3 and at most 24");
  }
  const std::uint32_t w = b.screen_scissor_br & 0xffff, h = b.screen_scissor_br >> 16;
  if (w == 0 || h == 0 || w > 2048 || h > 2048) return fail(r, "screen scissor gives an unusable target size");
  r.width = w; r.height = h; r.vertices = n;
  if (!b.viewport.present || std::fabs(std::fabs(b.viewport.xscale) - w / 2.0f) > 0.5f || std::fabs(std::fabs(b.viewport.yscale) - h / 2.0f) > 0.5f)
    return fail(r, "viewport scale does not match the target size (non-default viewports are not applied in this slice)");
  if (!mem.resolve(b.ps.address, 4)) return fail(r, "pixel shader address is not mapped: " + mem.last_error());
  // Slice S8: a PPS1 pixel blob is translated RDNA code producing a constant
  // draw colour from PS user data 0..7 (base rgba, intensity, ambient, clamp
  // bounds). Anything else keeps the fixed pass-through colour, stated below.
  bool pps1 = false;
  float ps_colour[4] = {0, 0, 0, 1};
  {
    std::uint32_t phdr[6];
    if (mem.read(b.ps.address, phdr, sizeof phdr) && phdr[0] == kPnrPsMagic && phdr[1] == 4) {
      std::vector<std::vector<std::uint32_t>> pk(4);
      std::uint64_t poff = sizeof phdr;
      for (int c = 0; c < 4; ++c) {
        if (phdr[2 + c] == 0 || phdr[2 + c] > 4096) return fail(r, "PPS1 kernel length is out of range");
        pk[c].resize(phdr[2 + c]);
        if (!mem.read(b.ps.address + poff, pk[c].data(), pk[c].size() * 4)) return fail(r, "PPS1 kernel runs past its mapping: " + mem.last_error());
        poff += pk[c].size() * 4;
      }
      std::vector<std::uint32_t> pu(8);
      for (int k = 0; k < 8; ++k) {
        if (!b.ps.user_data_set[k]) return fail(r, "PPS1 blob needs pixel user data 0..7 (base rgba, intensity, ambient, clamp bounds)");
        pu[k] = b.ps.user_data[k];
      }
      for (int c = 0; c < 4; ++c) {
        auto t = shader::translate_compute_words(pk[c], kRdnaPixelBase, 1, 1, 8);
        if (!t.ok) return fail(r, "RDNA pixel translation failed: " + t.error);
        std::vector<std::uint32_t> io(1, 0);
        auto st = shader::run_translated_compute(vk, t, &io, pu);
        if (!st) return fail(r, "translated pixel shader did not run: " + st.error.message);
        std::memcpy(&ps_colour[c], &io[0], 4);
      }
      pps1 = true;
    }
  }

  // Vertex-stage blob.
  std::uint32_t hdr[9];
  if (!mem.read(vs->address, hdr, sizeof hdr)) return fail(r, "vertex shader address is not mapped: " + mem.last_error());
  const bool pvs2 = hdr[0] == kPnrVs2Magic && hdr[1] == 7;
  if (!(hdr[0] == kPnrVsMagic && hdr[1] == 4) && !pvs2)
    return fail(r, "vertex shader memory is not a PNR PVS1/PVS2 blob (a real GCN/RDNA program is not accepted here)");
  const uint32_t nk = pvs2 ? 7 : 4;
  std::vector<std::vector<std::uint32_t>> kern(nk);
  std::uint64_t off = (2 + nk) * 4;
  for (uint32_t c = 0; c < nk; ++c) {
    if (hdr[2 + c] == 0 || hdr[2 + c] > 4096) return fail(r, "PVS blob kernel length is out of range");
    kern[c].resize(hdr[2 + c]);
    if (!mem.read(vs->address + off, kern[c].data(), kern[c].size() * 4)) return fail(r, "PVS blob kernel runs past its mapping: " + mem.last_error());
    off += kern[c].size() * 4;
  }
  std::uint64_t mat_va = 0, vb_va = 0;
  if (!read_u64(*vs, 0, &mat_va) || !read_u64(*vs, 2, &vb_va)) return fail(r, "vertex user data 0..3 (matrix and vertex buffer addresses) not set");
  std::vector<std::uint32_t> mat(16);
  std::vector<float> vb(std::size_t(n) * 6);
  if (!mem.read(mat_va, mat.data(), 64)) return fail(r, "matrix address is not mapped: " + mem.last_error());
  if (!mem.read(vb_va, vb.data(), vb.size() * 4)) return fail(r, "vertex buffer is not mapped: " + mem.last_error());

  // Translate and run each clip component.
  std::vector<float> clip(std::size_t(n) * 4, 0.0f);
  for (std::uint32_t c = 0; c < 4; ++c) {
    auto t = shader::translate_compute_words(kern[c], kRdnaVertexBase, n * 4, n, 16);
    if (!t.ok) return fail(r, "RDNA translation failed: " + t.error);
    std::vector<std::uint32_t> io(n * 4, 0);
    for (std::uint32_t i = 0; i < n; ++i)
      for (std::uint32_t k = 0; k < 3; ++k) std::memcpy(&io[k * n + i], &vb[i * 6 + k], 4);
    auto st = shader::run_translated_compute(vk, t, &io, mat);
    if (!st) return fail(r, "translated vertex shader did not run: " + st.error.message);
    for (std::uint32_t i = 0; i < n; ++i) std::memcpy(&clip[i * 4 + c], &io[3 * n + i], 4);
  }
  // Slice S6 (PVS2): the perspective divide runs in translated RDNA code,
  // one ndc component per kernel (the subset allows one store per kernel).
  // The rasterizer receives ndc with w = 1; viewport scale/offset on screen
  // stays host fixed-function.
  if (pvs2) {
    for (std::uint32_t c = 0; c < 3; ++c) {
      auto t = shader::translate_compute_words(kern[4 + c], kRdnaVertexBase, n * 5, n, 0);
      if (!t.ok) return fail(r, "RDNA post-transform translation failed: " + t.error);
      std::vector<std::uint32_t> io(std::size_t(n) * 5, 0);
      for (std::uint32_t i = 0; i < n; ++i)
        for (std::uint32_t k = 0; k < 4; ++k) std::memcpy(&io[k * n + i], &clip[i * 4 + k], 4);
      auto st = shader::run_translated_compute(vk, t, &io, {});
      if (!st) return fail(r, "translated post-transform did not run: " + st.error.message);
      for (std::uint32_t i = 0; i < n; ++i) std::memcpy(&clip[i * 4 + c], &io[4 * n + i], 4);
    }
    for (std::uint32_t i = 0; i < n; ++i) clip[i * 4 + 3] = 1.0f;
  }
  std::vector<float> verts;
  std::vector<std::uint32_t> idx;
  for (std::uint32_t i = 0; i < n; ++i) {
    for (int c = 0; c < 4; ++c) verts.push_back(clip[i * 4 + c]);
    for (int c = 3; c < 6; ++c) verts.push_back(pps1 ? ps_colour[c - 3] : vb[i * 6 + c]);
    idx.push_back(i);
  }
  if (draw.kind == pm4::DrawKind::Indexed) idx = indices;
  const float clear[4] = {0, 0, 0, 1};
  std::vector<std::uint32_t> vsp(std::begin(clip_vert), std::end(clip_vert)), fsp(std::begin(spin_frag), std::end(spin_frag));
  auto st = vk.render_clip_mesh_rgba(w, h, vsp, fsp, verts, idx, false, clear, &r.rgba);
  if (!st) return fail(r, "Vulkan draw failed: " + st.error.message);
  r.notes = pvs2 ? "PVS2 blob: clip and perspective divide are translated RDNA, w = 1 fed to the rasterizer"
                 : "vertex shader is a PNR PVS1 test blob";
  r.notes += pps1 ? "; pixel shader is a translated PPS1 blob (constant colour, no varyings)"
                  : "; pixel shader not translated: fixed pass-through colour used";
  if (draw.kind == pm4::DrawKind::Indexed) r.notes += "; indexed draw: index buffer fetched from guest memory";
  const std::size_t bytes = std::size_t(w) * h * 4;
  if (std::uint8_t* dst = mem.resolve_mut(b.color0.address, bytes)) {
    std::memcpy(dst, r.rgba.data(), bytes);
    r.wrote_target = true;
  } else {
    r.notes += "; colour target address not mapped for " + std::to_string(bytes) + " bytes, pixels returned only";
  }
  r.ok = true;
  return r;
}

}  // namespace pnr::graphics
