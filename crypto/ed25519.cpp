// See ed25519.hpp.
#include "crypto/ed25519.hpp"

#include <cstring>

namespace pnr::crypto {
namespace {
// ---- SHA-512 ----
const uint64_t K[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
inline uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
void block(uint64_t h[8], const uint8_t* p) {
  uint64_t w[80];
  for (int i = 0; i < 16; i++) { w[i] = 0; for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j]; }
  for (int i = 16; i < 80; i++) {
    uint64_t s0 = rotr(w[i-15],1) ^ rotr(w[i-15],8) ^ (w[i-15] >> 7);
    uint64_t s1 = rotr(w[i-2],19) ^ rotr(w[i-2],61) ^ (w[i-2] >> 6);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
  for (int i = 0; i < 80; i++) {
    uint64_t S1 = rotr(e,14) ^ rotr(e,18) ^ rotr(e,41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t t1 = hh + S1 + ch + K[i] + w[i];
    uint64_t S0 = rotr(a,28) ^ rotr(a,34) ^ rotr(a,39);
    uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint64_t t2 = S0 + mj;
    hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
struct Sha512 {
  uint64_t h[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                   0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
  uint8_t buf[128]; size_t n = 0; uint64_t total = 0;
  void add(const uint8_t* d, size_t len) {
    total += len;
    while (len) { size_t t = 128 - n < len ? 128 - n : len; memcpy(buf + n, d, t); n += t; d += t; len -= t; if (n == 128) { block(h, buf); n = 0; } }
  }
  void done(uint8_t out[64]) {
    uint64_t bits = total * 8; uint8_t pad = 0x80; add(&pad, 1); total -= 1;
    uint8_t z = 0; while (n != 112) { add(&z, 1); total -= 1; }
    uint8_t len[16] = {0}; for (int i = 0; i < 8; i++) len[15 - i] = (uint8_t)(bits >> (8 * i));
    add(len, 16);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(h[i] >> (56 - 8 * j));
  }
};

// ---- field GF(2^255-19), 16 limbs ----
typedef int64_t gf[16];
void set(gf r, const gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
void car(gf o) {
  for (int i = 0; i < 16; i++) { o[i] += (1LL << 16); int64_t c = o[i] >> 16; o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15); o[i] -= c << 16; }
}
void sel(gf p, gf q, int b) { int64_t c = ~(b - 1); for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; } }
void pack(uint8_t* o, const gf n) {
  gf m, t; for (int i = 0; i < 16; i++) t[i] = n[i];
  car(t); car(t); car(t);
  for (int j = 0; j < 2; j++) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int b = (int)((m[15] >> 16) & 1); m[14] &= 0xffff;
    sel(t, m, 1 - b);
  }
  for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
int neq(const gf a, const gf b) { uint8_t c[32], d[32]; pack(c, a); pack(d, b); return memcmp(c, d, 32) != 0; }
uint8_t par(const gf a) { uint8_t d[32]; pack(d, a); return d[0] & 1; }
void unpack(gf o, const uint8_t* n) { for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8); o[15] &= 0x7fff; }
void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
void M(gf o, const gf a, const gf b) {
  int64_t t[31] = {0};
  for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; i++) o[i] = t[i];
  car(o); car(o);
}
void S(gf o, const gf a) { M(o, a, a); }
void inv(gf o, const gf i) {
  gf c; set(c, i);
  for (int a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
  set(o, c);
}
void pow2523(gf o, const gf i) {
  gf c; set(c, i);
  for (int a = 250; a >= 0; a--) { S(c, c); if (a != 1) M(c, c, i); }
  set(o, c);
}
struct Consts {
  gf D, D2, I, X, Y;
  Consts() {
    gf one = {1}, t, u;
    // D = -121665 / 121666
    gf n = {0}, d = {0}; n[0] = 121665; d[0] = 121666;
    gf zero = {0}; Z(n, zero, n); inv(u, d); M(D, n, u); car(D);
    A(D2, D, D);
    // I = 2^((p-1)/4) = 2^(2^253-5): bits 252..0 set except bit 2
    gf two = {2}; set(t, one);
    for (int a = 252; a >= 0; a--) { S(t, t); if (a != 2) M(t, t, two); }
    set(I, t);
    // base point: y = 4/5, x even (compressed 0x58 0x66...)
    gf four = {4}, five = {5}; inv(u, five); M(Y, four, u);
    for (int i = 0; i < 16; i++) X[i] = 0;
  }
};
const Consts& C() { static const Consts c; return c; }

typedef gf point[4];
void add(point p, point q) {
  gf a, b, c, d, t, e, f, g, h;
  Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
  A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
  M(c, p[3], q[3]); M(c, c, C().D2);
  M(d, p[2], q[2]); A(d, d, d);
  Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
  M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}
void cswap(point p, point q, uint8_t b) { for (int i = 0; i < 4; i++) sel(p[i], q[i], b); }
void pack_pt(uint8_t* r, point p) {
  gf tx, ty, zi; inv(zi, p[2]); M(tx, p[0], zi); M(ty, p[1], zi);
  pack(r, ty); r[31] ^= par(tx) << 7;
}
void scalarmult(point p, point q, const uint8_t* s) {
  for (int i = 0; i < 16; i++) { p[0][i] = 0; p[1][i] = 0; p[2][i] = 0; p[3][i] = 0; }
  p[1][0] = 1; p[2][0] = 1;
  for (int i = 255; i >= 0; i--) {
    uint8_t b = (s[i / 8] >> (i & 7)) & 1;
    cswap(p, q, b); add(q, p); add(p, p); cswap(p, q, b);
  }
}
// r = -A where A is the decoded point; false if pk is not a valid point.
bool unpackneg(point r, const uint8_t p[32]) {
  gf t, chk, num, den, den2, den4, den6, zero = {0};
  for (int i = 0; i < 16; i++) r[2][i] = 0;
  r[2][0] = 1;
  unpack(r[1], p);
  S(num, r[1]); M(den, num, C().D); Z(num, num, r[2]); A(den, r[2], den);
  S(den2, den); S(den4, den2); M(den6, den4, den2); M(t, den6, num); M(t, t, den);
  pow2523(t, t); M(t, t, num); M(t, t, den); M(t, t, den); M(r[0], t, den);
  S(chk, r[0]); M(chk, chk, den);
  if (neq(chk, num)) M(r[0], r[0], C().I);
  S(chk, r[0]); M(chk, chk, den);
  if (neq(chk, num)) return false;
  if (par(r[0]) == (p[31] >> 7)) Z(r[0], zero, r[0]);
  M(r[3], r[0], r[1]);
  return true;
}
const uint8_t Lb[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
void modL(uint8_t* r, int64_t x[64]) {
  int64_t carry, i, j;
  for (i = 63; i >= 32; --i) {
    carry = 0;
    for (j = i - 32; j < i - 12; ++j) { x[j] += carry - 16 * x[i] * Lb[j - (i - 32)]; carry = (x[j] + 128) >> 8; x[j] -= carry << 8; }
    x[j] += carry; x[i] = 0;
  }
  carry = 0;
  for (j = 0; j < 32; ++j) { x[j] += carry - (x[31] >> 4) * Lb[j]; carry = x[j] >> 8; x[j] &= 255; }
  for (j = 0; j < 32; ++j) x[j] -= carry * Lb[j];
  for (i = 0; i < 32; ++i) { x[i + 1] += x[i] >> 8; r[i] = (uint8_t)(x[i] & 255); }
}
bool s_canonical(const uint8_t* s) {
  for (int i = 31; i >= 0; i--) { if (s[i] < Lb[i]) return true; if (s[i] > Lb[i]) return false; }
  return false;  // equal to L
}
}  // namespace

void sha512(const uint8_t* data, size_t len, uint8_t out[64]) { Sha512 h; h.add(data, len); h.done(out); }

bool ed25519_verify(const uint8_t pk[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
  if (!s_canonical(sig + 32)) return false;
  // base point B: y = 4/5, x even => compressed 0x58 0x66 x30 0x66
  uint8_t benc[32]; memset(benc, 0x66, 32); benc[0] = 0x58;
  point nb, na, p, q;
  if (!unpackneg(nb, benc)) return false;
  gf zero = {0};
  Z(nb[0], zero, nb[0]); Z(nb[3], zero, nb[3]);  // negate -B to get B
  if (!unpackneg(na, pk)) return false;           // na = -A
  Sha512 h; h.add(sig, 32); h.add(pk, 32); h.add(msg, len);
  uint8_t dig[64]; h.done(dig);
  int64_t x[64]; for (int i = 0; i < 64; i++) x[i] = dig[i];
  uint8_t k[32]; modL(k, x);
  scalarmult(p, na, k);       // p = [k](-A)
  scalarmult(q, nb, sig + 32);  // q = [S]B
  add(p, q);
  uint8_t t[32]; pack_pt(t, p);
  uint8_t d = 0; for (int i = 0; i < 32; i++) d |= t[i] ^ sig[i];
  return d == 0;
}

namespace {
void make_base(point b) {
  uint8_t benc[32]; memset(benc, 0x66, 32); benc[0] = 0x58;
  unpackneg(b, benc);
  gf zero = {0}; Z(b[0], zero, b[0]); Z(b[3], zero, b[3]);
}
}  // namespace

void ed25519_pubkey(const uint8_t seed[32], uint8_t pk[32]) {
  uint8_t d[64]; sha512(seed, 32, d); d[0] &= 248; d[31] &= 127; d[31] |= 64;
  point b, p; make_base(b); scalarmult(p, b, d); pack_pt(pk, p);
}

void ed25519_sign(const uint8_t seed[32], const uint8_t* msg, size_t len, uint8_t sig[64]) {
  uint8_t d[64], pk[32]; sha512(seed, 32, d); d[0] &= 248; d[31] &= 127; d[31] |= 64;
  { point b, p; make_base(b); scalarmult(p, b, d); pack_pt(pk, p); }
  uint8_t r[32], k[32], dig[64]; int64_t x[64];
  { Sha512 h; h.add(d + 32, 32); h.add(msg, len); h.done(dig); }
  for (int i = 0; i < 64; i++) x[i] = dig[i];
  modL(r, x);
  { point b, p; make_base(b); scalarmult(p, b, r); pack_pt(sig, p); }
  { Sha512 h; h.add(sig, 32); h.add(pk, 32); h.add(msg, len); h.done(dig); }
  for (int i = 0; i < 64; i++) x[i] = dig[i];
  modL(k, x);
  for (int i = 0; i < 64; i++) x[i] = 0;
  for (int i = 0; i < 32; i++) x[i] = r[i];
  for (int i = 0; i < 32; i++) for (int j = 0; j < 32; j++) x[i + j] += (int64_t)k[i] * d[j];
  modL(sig + 32, x);
}

bool hex_decode(const std::string& hex, std::string* out) {
  if (hex.size() % 2) return false;
  out->clear();
  auto v = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
  for (size_t i = 0; i < hex.size(); i += 2) { int a = v(hex[i]), b = v(hex[i + 1]); if (a < 0 || b < 0) return false; out->push_back((char)(a * 16 + b)); }
  return true;
}
std::string hex_encode(const uint8_t* d, size_t n) {
  static const char* H = "0123456789abcdef"; std::string s;
  for (size_t i = 0; i < n; i++) { s.push_back(H[d[i] >> 4]); s.push_back(H[d[i] & 15]); }
  return s;
}
}  // namespace pnr::crypto
