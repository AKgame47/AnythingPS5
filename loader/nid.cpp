// Adapted from AnyPS5 (GPL-2.0): core/libs/nid/src/Sha1.cpp and
// core/libs/nid/src/NidCompute.cpp. https://github.com/boykopovar/AnyPS5
#include "loader/nid.hpp"

#include <stdexcept>

namespace pnr::loader::nid {

namespace {
std::uint32_t rotl(std::uint32_t v, unsigned n) {
  return (v << n) | (v >> (32u - n));
}
}  // namespace

std::array<std::uint8_t, 20> sha1(const std::vector<std::uint8_t>& data) {
  std::uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu,
                h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
  std::vector<std::uint8_t> msg(data);
  const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
  msg.push_back(0x80u);
  while ((msg.size() % 64u) != 56u) msg.push_back(0x00u);
  for (int i = 7; i >= 0; --i)
    msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8u)) & 0xffu));

  for (std::size_t off = 0; off < msg.size(); off += 64u) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<std::uint32_t>(msg[off + i * 4 + 0]) << 24u) |
             (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 16u) |
             (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 8u) |
             static_cast<std::uint32_t>(msg[off + i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);
    std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const std::uint32_t temp = rotl(a, 5u) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30u); b = a; a = temp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  std::array<std::uint8_t, 20> digest{};
  auto put = [&](int base, std::uint32_t v) {
    digest[base + 0] = static_cast<std::uint8_t>(v >> 24u);
    digest[base + 1] = static_cast<std::uint8_t>(v >> 16u);
    digest[base + 2] = static_cast<std::uint8_t>(v >> 8u);
    digest[base + 3] = static_cast<std::uint8_t>(v);
  };
  put(0, h0); put(4, h1); put(8, h2); put(12, h3); put(16, h4);
  return digest;
}

std::string compute_nid(const std::string& symbol_name,
                        const std::string& /*library_name*/) {
  if (symbol_name.empty())
    throw std::invalid_argument("symbol_name is empty");
  static constexpr std::uint8_t kSuffix[16] = {
      0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
      0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
  static constexpr char kBase64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
  std::vector<std::uint8_t> input;
  input.reserve(symbol_name.size() + 16u);
  for (char c : symbol_name) input.push_back(static_cast<std::uint8_t>(c));
  for (std::uint8_t b : kSuffix) input.push_back(b);
  const auto digest = sha1(input);
  std::uint8_t reversed[8];
  for (int i = 0; i < 8; i++) reversed[i] = digest[7 - i];
  char nid[12];
  int idx = 0;
  for (int i = 0; i < 6; i += 3) {
    const std::uint32_t triple = (static_cast<std::uint32_t>(reversed[i]) << 16) |
                                 (static_cast<std::uint32_t>(reversed[i + 1]) << 8) |
                                 static_cast<std::uint32_t>(reversed[i + 2]);
    nid[idx++] = kBase64[(triple >> 18) & 0x3F];
    nid[idx++] = kBase64[(triple >> 12) & 0x3F];
    nid[idx++] = kBase64[(triple >> 6) & 0x3F];
    nid[idx++] = kBase64[triple & 0x3F];
  }
  const std::uint32_t tail = (static_cast<std::uint32_t>(reversed[6]) << 16) |
                             (static_cast<std::uint32_t>(reversed[7]) << 8);
  nid[idx++] = kBase64[(tail >> 18) & 0x3F];
  nid[idx++] = kBase64[(tail >> 12) & 0x3F];
  nid[idx++] = kBase64[(tail >> 6) & 0x3F];
  nid[11] = '\0';
  return std::string(nid, 11);
}

std::string resolve_nid(const std::string& nid_str) {
  // Seed table of common symbols; values computed by compute_nid().
  static const char* kCommon[] = {
      "printf", "malloc", "free", "memcpy", "memset", "strlen", "strcmp",
      "sceKernelAlloc", "sceKernelFree", "sceKernelSleep",
      "sceKernelCreateThread", "sceKernelUsleep"};
  for (const char* s : kCommon)
    if (compute_nid(s, "") == nid_str) return s;
  return {};
}

}  // namespace pnr::loader::nid
