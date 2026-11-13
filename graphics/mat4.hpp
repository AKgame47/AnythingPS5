// Minimal column-major 4x4 float math for the PNR graphics demos, GPL-2.0.
// Conventions: right-handed view space looking down -Z, Vulkan clip space
// (y down, depth 0..1). m[col*4+row], so the array can be pushed as-is to a
// shader declaring a column-major mat4.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pnr::graphics {

using Mat4 = std::array<float, 16>;

inline Mat4 mat4_identity() {
  Mat4 m{};
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  return m;
}
inline Mat4 mat4_mul(const Mat4& a, const Mat4& b) {  // a * b
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  return r;
}
inline Mat4 mat4_translate(float x, float y, float z) {
  Mat4 m = mat4_identity();
  m[12] = x; m[13] = y; m[14] = z;
  return m;
}
inline Mat4 mat4_rotate_y(float a) {
  Mat4 m = mat4_identity();
  const float c = std::cos(a), s = std::sin(a);
  m[0] = c; m[8] = s; m[2] = -s; m[10] = c;
  return m;
}
inline Mat4 mat4_rotate_x(float a) {
  Mat4 m = mat4_identity();
  const float c = std::cos(a), s = std::sin(a);
  m[5] = c; m[9] = -s; m[6] = s; m[10] = c;
  return m;
}
// Vulkan-style perspective: y flipped (world +y is up on screen), depth 0..1.
inline Mat4 mat4_perspective(float fovy, float aspect, float zn, float zf) {
  const float f = 1.0f / std::tan(fovy * 0.5f);
  Mat4 m{};
  m[0] = f / aspect;
  m[5] = -f;
  m[10] = zf / (zn - zf);
  m[11] = -1.0f;
  m[14] = zn * zf / (zn - zf);
  return m;
}
// Transform a point (w=1); returns clip-space x,y,z,w.
inline std::array<float, 4> mat4_apply(const Mat4& m, float x, float y, float z) {
  std::array<float, 4> r{};
  for (int row = 0; row < 4; ++row)
    r[row] = m[0 * 4 + row] * x + m[1 * 4 + row] * y +
                                       m[2 * 4 + row] * z + m[3 * 4 + row];
  return r;
}
inline std::vector<uint32_t> mat4_words(const Mat4& m) {
  std::vector<uint32_t> w(16);
  std::memcpy(w.data(), m.data(), 64);
  return w;
}

// Unit cube centred on the origin: 24 vertices (4 per face, flat face
// colour), 36 indices. Faces in index order: +Z red, +X green, +Y blue,
// -Z yellow, -X magenta, -Y cyan. Vertex layout: x y z r g b.
inline void cube_mesh(std::vector<float>* v, std::vector<uint32_t>* idx) {
  struct Face { float n[3], u[3], w[3], c[3]; };
  const Face faces[6] = {
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}},
      {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}},
      {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}},
      {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {1, 1, 0}},
      {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 1}},
      {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 1}}};
  v->clear(); idx->clear();
  for (int f = 0; f < 6; ++f) {
    const Face& F = faces[f];
    const float su[4] = {-1, 1, 1, -1}, sw[4] = {-1, -1, 1, 1};
    const uint32_t base = (uint32_t)(v->size() / 6);
    for (int i = 0; i < 4; ++i) {
      for (int k = 0; k < 3; ++k)
        v->push_back(0.5f * (F.n[k] + su[i] * F.u[k] + sw[i] * F.w[k]));
      for (int k = 0; k < 3; ++k) v->push_back(F.c[k]);
    }
    for (uint32_t t : {0u, 1u, 2u, 0u, 2u, 3u}) idx->push_back(base + t);
  }
}

}  // namespace pnr::graphics
