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

std::vector<std::uint32_t> make_pnr_vs3_blob(bool with_post) {
  std::vector<std::vector<std::uint32_t>> k;
  for (std::uint32_t c = 0; c < 4; ++c) k.push_back(rdna_vertex_component_kernel(c, kRdnaVertexMax));
  if (with_post) for (std::uint32_t c = 0; c < 3; ++c) k.push_back(rdna_post_ndc_kernel(c, kRdnaVertexMax));
  std::vector<std::uint32_t> blob = {kPnrVs3Magic, static_cast<std::uint32_t>(k.size())};
  for (auto& v : k) blob.push_back(static_cast<std::uint32_t>(v.size()));
  for (auto& v : k) blob.insert(blob.end(), v.begin(), v.end());
  return blob;
}

std::vector<std::uint32_t> make_pnr_vs4_blob() {
  std::vector<std::vector<std::uint32_t>> k;
  for (std::uint32_t c = 0; c < 4; ++c) k.push_back(rdna_vertex_component_kernel(c, kRdnaVertexMax));
  for (std::uint32_t c = 0; c < 3; ++c) k.push_back(rdna_shade_kernel(c, kRdnaVertexMax));
  std::vector<std::uint32_t> blob = {kPnrVs4Magic, 7};
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

DrawFrameCache::~DrawFrameCache() {
  if (vk && surface.color_image != 0) vk->destroy_frame(surface);
}

DrawResult execute_draw(VulkanBackend& vk, GpuAddressSpace& mem, const pm4::DrawCall& draw) {
  DrawFrameCache frame;  // empty: every draw starts a fresh frame (per-draw depth)
  return execute_draw(vk, mem, draw, frame);
}

DrawResult execute_draw(VulkanBackend& vk, GpuAddressSpace& mem, const pm4::DrawCall& draw,
                        DrawFrameCache& frame) {
  DrawResult r;
  if (draw.kind != pm4::DrawKind::Auto && draw.kind != pm4::DrawKind::Indexed)
    return fail(r, "only DRAW_INDEX_AUTO and DRAW_INDEX_INDX are supported");
  if (draw.instances != 1) return fail(r, "only one instance is supported");
  const DrawBindings b = extract_bindings(draw);
  if (!b.missing.empty()) return fail(r, "draw state incomplete: " + b.missing);
  const bool trilist = b.primitive == amd::PrimitiveType::TriangleList;
  const bool tristrip = b.primitive == amd::PrimitiveType::TriangleStrip;
  const bool trifan = b.primitive == amd::PrimitiveType::TriangleFan;
  if (!trilist && !tristrip && !trifan) return fail(r, "only triangle lists, strips and fans are supported");
  const ShaderBinding* vs = b.vertex_stage();
  if (!vs) return fail(r, "no vertex-stage shader address set");
  if (!b.ps.present) return fail(r, "no pixel-stage shader address set");
  if (!b.color0.present || !b.has_scissor) return fail(r, "colour target 0 or screen scissor not set");
  // Slice S12: depth testing. DB_DEPTH_CONTROL bit 1 switches on the host
  // depth attachment; only the LESS compare (bits 4..6 = 1) is supported,
  // stencil (bit 0) is refused. The depth surface is per-draw: it is created
  // and destroyed with this draw, so there is no cross-draw depth and no
  // depth readback, and DB_DEPTH_CLEAR/compare selection beyond LESS are not
  // tracked yet (the host clears to 1.0).
  bool use_depth = false;
  bool depth_write = true;
  std::uint64_t z_base = 0;
  if (b.depth_control_set) {
    if (b.depth_control & 1) return fail(r, "stencil testing is not supported");
    if (b.depth_control & 2) {
      if (((b.depth_control >> 4) & 7) != 1) return fail(r, "only the LESS depth compare function is supported");
      use_depth = true;
      depth_write = (b.depth_control & 4) != 0;
    }
  }
  // Slice S13: a bound DB depth buffer (DB_Z_INFO + bases + size) turns the
  // depth attachment into a frame-persistent surface shared with the colour
  // target via the caller's DrawFrameCache. Only a linear Z32_FLOAT surface
  // is supported; tiled/HTILE, Z16, stencil and separate read/write bases
  // are refused. Without a valid binding the draw keeps the slice-S12
  // per-draw depth surface. The surface clears when the binding or size
  // changes; DB render-control clears and DB_DEPTH_CLEAR are not tracked.
  if (use_depth && b.depth_buffer_set) {
    const std::uint32_t zfmt = b.db_z_info & 3;
    if (zfmt == 1) return fail(r, "16-bit depth surfaces are not supported (Z32 float only)");
    if (zfmt == 2) return fail(r, "reserved depth buffer format");
    if ((b.db_z_info >> 28) & 1) return fail(r, "tiled depth surfaces (HTILE) are not supported; linear only");
    if (b.db_stencil_info & 1) return fail(r, "stencil testing is not supported");
    if (zfmt == 3 && b.db_z_read_base != 0) {
      if (depth_write && b.db_z_write_base != 0 && b.db_z_write_base != b.db_z_read_base)
        return fail(r, "separate depth read/write bases are not supported");
      z_base = b.db_z_read_base;
    }
  }
  // Slice S14: CB_BLEND0_CONTROL bit 30 switches on target-0 blending.
  // GNM orders factors 4..9 alpha-first (4 src-alpha .. 7 inv-dst-alpha,
  // 8 dst-colour, 9 inv-dst-colour); Vulkan orders them colour-first
  // (4 dst-colour, 5 inv-dst-colour, 6 src-alpha .. 9 inv-dst-alpha), so
  // values 4..9 translate through a small table. All five GNM blend funcs
  // have Vulkan equivalents. Constant colour/alpha, dual-source (src1) and
  // src-alpha-saturate factors are refused (no blend constants or
  // dual-source outputs yet). Bit 29 selects separate alpha factors;
  // without it the colour factors apply to alpha. Fragment alpha is 1.0 on
  // the current pipeline (no vertex alpha, no pixel-shader alpha export),
  // stated in the notes.
  BlendState blend;
  if (b.blend0_set && ((b.blend0 >> 30) & 1)) {
    auto factor = [](std::uint32_t f, int32_t* out) -> bool {
      // GNM factor value -> VK_BLEND_FACTOR_* (alpha/colour rows 4..9 swap).
      static const int32_t kToVulkan[10] = {
          VK_BLEND_FACTOR_ZERO,
          VK_BLEND_FACTOR_ONE,
          VK_BLEND_FACTOR_SRC_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
          VK_BLEND_FACTOR_SRC_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          VK_BLEND_FACTOR_DST_ALPHA,
          VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
          VK_BLEND_FACTOR_DST_COLOR,
          VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      };
      if (f > 9) return false;
      *out = kToVulkan[f];
      return true;
    };
    auto func = [](std::uint32_t f, int32_t* out) -> bool {
      switch (f) {  // GNM: 0 add, 1 sub, 2 min, 3 max, 4 reverse-sub
        case 0: *out = VK_BLEND_OP_ADD; return true;
        case 1: *out = VK_BLEND_OP_SUBTRACT; return true;
        case 2: *out = VK_BLEND_OP_MIN; return true;
        case 3: *out = VK_BLEND_OP_MAX; return true;
        case 4: *out = VK_BLEND_OP_REVERSE_SUBTRACT; return true;
      }
      return false;
    };
    const std::uint32_t csf = b.blend0 & 31, cf = (b.blend0 >> 5) & 7, cdf = (b.blend0 >> 8) & 31;
    const std::uint32_t asf = (b.blend0 >> 16) & 31, af = (b.blend0 >> 21) & 7, adf = (b.blend0 >> 24) & 31;
    const bool sep = (b.blend0 >> 29) & 1;
    blend.enable = true;
    if (!factor(csf, &blend.src_color) || !factor(cdf, &blend.dst_color) ||
        !factor(sep ? asf : csf, &blend.src_alpha) || !factor(sep ? adf : cdf, &blend.dst_alpha))
      return fail(r, "blend factor not supported (constant colour/alpha, dual-source and src-alpha-saturate are not implemented)");
    if (!func(cf, &blend.color_op) || !func(sep ? af : cf, &blend.alpha_op))
      return fail(r, "blend function not supported");
  }
  // Slice S7: indexed draws fetch the index buffer from guest memory. The
  // vertex transform runs over the referenced vertices (max index + 1);
  // indices pass through unchanged. Slice S9: PVS1/PVS2 blobs run one
  // dispatch (at most 64 vertices); PVS3 blobs batch any count up to
  // kDrawMaxVertices in 64-lane dispatches.
  std::vector<std::uint32_t> indices;
  std::uint32_t n = draw.count;
  if (draw.kind == pm4::DrawKind::Indexed) {
    if (draw.index_size_bytes != 2 && draw.index_size_bytes != 4)
      return fail(r, "index type not set before the draw (16-bit and 32-bit indices are supported)");
    if (draw.count < 3)
      return fail(r, "index count must be at least 3");
    if (trilist && draw.count % 3)
      return fail(r, "triangle-list index count must be a multiple of 3");
    if (draw.count > kDrawMaxIndices)
      return fail(r, "index count is over the draw cap of " + std::to_string(kDrawMaxIndices));
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
    if (n > kDrawMaxVertices)
      return fail(r, "indexed draw references more than " + std::to_string(kDrawMaxVertices) + " vertices (draw cap)");
  } else if (n < 3 || (trilist && n % 3) || n > kDrawMaxVertices) {
    return fail(r, trilist ? "vertex count must be a multiple of 3 and at most " + std::to_string(kDrawMaxVertices)
                           : "vertex count must be at least 3 and at most " + std::to_string(kDrawMaxVertices));
  }
  // Slice S10: triangle strips and fans expand to a plain triangle list on
  // the host before rasterization. Strip triangle i takes source vertices
  // (i, i+1, i+2) with the first two swapped on odd i to keep the winding;
  // fan triangle i takes (0, i+1, i+2). Winding matters once culling exists;
  // the current rasterizer does not cull, so this is a correctness pre-step.
  if (!trilist) {
    if (draw.count - 2 > kDrawMaxIndices / 3)
      return fail(r, "expanded index count is over the draw cap of " + std::to_string(kDrawMaxIndices));
    std::vector<std::uint32_t> seq;
    if (draw.kind == pm4::DrawKind::Indexed) seq = indices;
    else { seq.resize(n); for (std::uint32_t i = 0; i < n; ++i) seq[i] = i; }
    indices.clear();
    indices.reserve(std::size_t(draw.count - 2) * 3);
    for (std::uint32_t i = 0; i + 2 < draw.count; ++i) {
      if (trifan) {
        indices.push_back(seq[0]); indices.push_back(seq[i + 1]); indices.push_back(seq[i + 2]);
      } else if (i & 1) {
        indices.push_back(seq[i + 1]); indices.push_back(seq[i]); indices.push_back(seq[i + 2]);
      } else {
        indices.push_back(seq[i]); indices.push_back(seq[i + 1]); indices.push_back(seq[i + 2]);
      }
    }
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
  const bool pvs3 = hdr[0] == kPnrVs3Magic && (hdr[1] == 4 || hdr[1] == 7);
  const bool pvs4 = hdr[0] == kPnrVs4Magic && hdr[1] == 7;
  if (!(hdr[0] == kPnrVsMagic && hdr[1] == 4) && !pvs2 && !pvs3 && !pvs4)
    return fail(r, "vertex shader memory is not a PNR PVS1/PVS2/PVS3/PVS4 blob (a real GCN/RDNA program is not accepted here)");
  if (!pvs3 && !pvs4 && n > kRdnaVertexMax)
    return fail(r, "PVS1/PVS2 blobs are built per vertex count and run one dispatch (at most 64 vertices); a PVS3/PVS4 blob batches larger draws");
  const uint32_t nk = (pvs2 || pvs3 || pvs4) ? hdr[1] : 4;
  const bool batched = pvs3 || pvs4;
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
  const std::uint32_t stride = pvs4 ? 9 : 6;   // PVS4 adds per-vertex normals (slice S11)
  std::vector<float> vb(std::size_t(n) * stride);
  if (!mem.read(mat_va, mat.data(), 64)) return fail(r, "matrix address is not mapped: " + mem.last_error());
  if (!mem.read(vb_va, vb.data(), vb.size() * 4)) return fail(r, "vertex buffer is not mapped: " + mem.last_error());
  std::vector<std::uint32_t> shade_params(6);
  if (pvs4) {
    for (int k = 0; k < 6; ++k) {
      if (!vs->user_data_set[4 + k]) return fail(r, "PVS4 blob needs vertex user data 4..9 (light xyz, ambient, clamp lo/hi)");
      shade_params[k] = vs->user_data[4 + k];
    }
  }

  // Translate and run each clip component. PVS1/PVS2 kernels are specialized
  // to n (one dispatch). PVS3 kernels are built for the fixed 64-lane
  // workgroup, so any n runs as ceil(n/64) dispatches; the last batch
  // zero-pads its tail lanes and their stores land in scratch (slice S9).
  const std::uint32_t lanes = batched ? kRdnaVertexMax : n;
  const std::uint32_t batches = batched ? (n + lanes - 1) / lanes : 1;
  std::vector<float> clip(std::size_t(n) * 4, 0.0f);
  for (std::uint32_t c = 0; c < 4; ++c) {
    auto t = shader::translate_compute_words(kern[c], kRdnaVertexBase, lanes * 4, lanes, 16);
    if (!t.ok) return fail(r, "RDNA translation failed: " + t.error);
    for (std::uint32_t bno = 0; bno < batches; ++bno) {
      const std::uint32_t base = bno * lanes, m = (n - base < lanes) ? (n - base) : lanes;
      std::vector<std::uint32_t> io(std::size_t(lanes) * 4, 0);
      for (std::uint32_t i = 0; i < m; ++i)
        for (std::uint32_t k = 0; k < 3; ++k) std::memcpy(&io[k * lanes + i], &vb[(base + i) * stride + k], 4);
      auto st = shader::run_translated_compute(vk, t, &io, mat);
      if (!st) return fail(r, "translated vertex shader did not run: " + st.error.message);
      for (std::uint32_t i = 0; i < m; ++i) std::memcpy(&clip[(base + i) * 4 + c], &io[3 * lanes + i], 4);
    }
  }
  // Slice S6 (PVS2): the perspective divide runs in translated RDNA code,
  // one ndc component per kernel (the subset allows one store per kernel).
  // The rasterizer receives ndc with w = 1; viewport scale/offset on screen
  // stays host fixed-function. PVS3 with 7 kernels runs the same divide in
  // 64-lane batches.
  const bool with_post = pvs2 || (pvs3 && nk == 7);
  if (with_post) {
    for (std::uint32_t c = 0; c < 3; ++c) {
      auto t = shader::translate_compute_words(kern[4 + c], kRdnaVertexBase, lanes * 5, lanes, 0);
      if (!t.ok) return fail(r, "RDNA post-transform translation failed: " + t.error);
      for (std::uint32_t bno = 0; bno < batches; ++bno) {
        const std::uint32_t base = bno * lanes, m = (n - base < lanes) ? (n - base) : lanes;
        std::vector<std::uint32_t> io(std::size_t(lanes) * 5, 0);
        for (std::uint32_t i = 0; i < m; ++i)
          for (std::uint32_t k = 0; k < 4; ++k) std::memcpy(&io[k * lanes + i], &clip[(base + i) * 4 + k], 4);
        auto st = shader::run_translated_compute(vk, t, &io, {});
        if (!st) return fail(r, "translated post-transform did not run: " + st.error.message);
        for (std::uint32_t i = 0; i < m; ++i) std::memcpy(&clip[(base + i) * 4 + c], &io[4 * lanes + i], 4);
      }
    }
    for (std::uint32_t i = 0; i < n; ++i) clip[i * 4 + 3] = 1.0f;
  }
  // Slice S11 (PVS4): three shade kernels compute the per-vertex colour
  // varying in translated RDNA (dot + ambient, clamped, times the vertex
  // colour); the rasterizer interpolates the result across each triangle.
  std::vector<float> shade;
  if (pvs4) {
    shade.assign(std::size_t(n) * 3, 0.0f);
    for (std::uint32_t c = 0; c < 3; ++c) {
      auto t = shader::translate_compute_words(kern[4 + c], kRdnaVertexBase, lanes * 8, lanes, 6);
      if (!t.ok) return fail(r, "RDNA shade translation failed: " + t.error);
      for (std::uint32_t bno = 0; bno < batches; ++bno) {
        const std::uint32_t base = bno * lanes, m = (n - base < lanes) ? (n - base) : lanes;
        std::vector<std::uint32_t> io(std::size_t(lanes) * 8, 0);
        for (std::uint32_t i = 0; i < m; ++i) {
          for (std::uint32_t k = 0; k < 3; ++k) std::memcpy(&io[k * lanes + i], &vb[(base + i) * 9 + k], 4);
          for (std::uint32_t k = 0; k < 3; ++k) std::memcpy(&io[(3 + k) * lanes + i], &vb[(base + i) * 9 + 6 + k], 4);
          std::memcpy(&io[6 * lanes + i], &vb[(base + i) * 9 + 3 + c], 4);
        }
        auto st = shader::run_translated_compute(vk, t, &io, shade_params);
        if (!st) return fail(r, "translated shade kernel did not run: " + st.error.message);
        for (std::uint32_t i = 0; i < m; ++i) std::memcpy(&shade[(base + i) * 3 + c], &io[7 * lanes + i], 4);
      }
    }
  }
  std::vector<float> verts;
  std::vector<std::uint32_t> idx;
  for (std::uint32_t i = 0; i < n; ++i) {
    for (int c = 0; c < 4; ++c) verts.push_back(clip[i * 4 + c]);
    for (int c = 0; c < 3; ++c) verts.push_back(pps1 ? ps_colour[c] : pvs4 ? shade[i * 3 + c] : vb[i * stride + 3 + c]);
    idx.push_back(i);
  }
  if (!indices.empty()) idx = indices;
  const float clear[4] = {0, 0, 0, 1};
  std::vector<std::uint32_t> vsp(std::begin(clip_vert), std::end(clip_vert)), fsp(std::begin(spin_frag), std::end(spin_frag));
  // Slice S13: hand the caller's frame cache to the renderer when a depth
  // buffer is bound; a changed colour/depth binding or size restarts the
  // frame (fresh clear). The colour target always comes from the cache so
  // consecutive draws accumulate; depth joins the surface when bound.
  const std::uint64_t color_base = b.color0.address;
  const bool frame_changed = frame.color_base != color_base || frame.z_base != z_base ||
                             frame.width != w || frame.height != h;
  frame.vk = &vk;
  frame.color_base = color_base; frame.z_base = z_base; frame.width = w; frame.height = h;
  (void)frame_changed;  // the renderer restarts the frame on size change; binding change is below
  if (frame.surface.color_image != 0 && frame_changed) vk.destroy_frame(frame.surface);
  auto st = vk.render_clip_mesh_rgba(w, h, vsp, fsp, verts, idx, use_depth, clear, &r.rgba,
                                     &frame.surface, depth_write, blend.enable ? &blend : nullptr);
  if (!st) return fail(r, "Vulkan draw failed: " + st.error.message);
  r.notes = pvs4 ? "PVS4 blob: clip and Gouraud shade varyings are translated RDNA in 64-lane batches (" + std::to_string(batches) + " for " + std::to_string(n) + " vertices); the rasterizer interpolates the shade"
                 : pvs3 ? (with_post ? "PVS3 blob: clip and perspective divide are translated RDNA in 64-lane batches (" + std::to_string(batches) + " for " + std::to_string(n) + " vertices), w = 1 fed to the rasterizer"
                              : "PVS3 blob: clip transform is translated RDNA in 64-lane batches (" + std::to_string(batches) + " for " + std::to_string(n) + " vertices)")
                 : pvs2 ? "PVS2 blob: clip and perspective divide are translated RDNA, w = 1 fed to the rasterizer"
                        : "vertex shader is a PNR PVS1 test blob";
  r.notes += pps1 ? "; pixel shader is a translated PPS1 blob (constant colour, no varyings)"
                  : "; pixel shader not translated: fixed pass-through colour used";
  if (draw.kind == pm4::DrawKind::Indexed) r.notes += "; indexed draw: index buffer fetched from guest memory";
  if (tristrip) r.notes += "; triangle strip expanded to a list on the host (winding corrected)";
  if (trifan) r.notes += "; triangle fan expanded to a list on the host";
  if (use_depth && z_base)
    r.notes += "; depth testing on (LESS, frame-persistent depth surface, no readback)";
  else if (use_depth)
    r.notes += "; depth testing on (LESS, per-draw depth surface, no cross-draw depth or readback)";
  if (frame.surface.color_image != 0)
    r.notes += "; shared frame targets (draws accumulate into the bound colour target)";
  if (blend.enable)
    r.notes += "; target-0 blending on (CB_BLEND0_CONTROL; fragment alpha is 1.0 on this pipeline)";
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
