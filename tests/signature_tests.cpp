// Slice B1 tests: Ed25519 (RFC 8032 section 7.1 vectors), SHA-512, and the
// store signature policy (pinned key => signature required and verified).
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "crypto/ed25519.hpp"
#include "store/download.hpp"
#include "store/store_cli.hpp"

namespace fs = std::filesystem;
using namespace pnr::store;
namespace {
std::string hx(const std::string& h) { std::string o; pnr::crypto::hex_decode(h, &o); return o; }
bool vfy(const std::string& pk, const std::string& m, const std::string& sig) {
  return pnr::crypto::ed25519_verify(reinterpret_cast<const uint8_t*>(pk.data()), reinterpret_cast<const uint8_t*>(m.data()), m.size(), reinterpret_cast<const uint8_t*>(sig.data()));
}
}  // namespace

void pnr_signature_tests(int& pass, int& fail) {
  auto check = [&](bool ok, const std::string& label) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str()); };
  namespace C = pnr::crypto;
  uint8_t d[64];
  C::sha512(reinterpret_cast<const uint8_t*>("abc"), 3, d);
  check(C::hex_encode(d, 64) == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "sha512(abc) matches FIPS 180-4");
  C::sha512(nullptr, 0, d);
  check(C::hex_encode(d, 64).rfind("cf83e1357eefb8bd", 0) == 0, "sha512(empty) prefix matches");
  const std::string pk1 = hx("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  const std::string sg1 = hx("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  const std::string pk2 = hx("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
  const std::string sg2 = hx("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
  check(vfy(pk1, "", sg1), "RFC 8032 vector 1 (empty message) verifies");
  check(vfy(pk2, std::string("\x72"), sg2), "RFC 8032 vector 2 (1 byte) verifies");
  check(!vfy(pk1, std::string("\x00", 1), sg1), "wrong message is rejected");
  check(!vfy(pk2, "", sg1), "wrong key is rejected");
  { bool all = true; for (int i = 0; i < 64; i += 7) { std::string b = sg1; b[i] ^= 1; all = all && !vfy(pk1, "", b); } check(all, "bit-flipped signatures are rejected"); }
  { std::string b = pk1; b[3] ^= 1; check(!vfy(b, "", sg1), "bit-flipped public key is rejected"); }
  { // S + L is a different non-canonical encoding of the same scalar: must be refused
    static const uint8_t L[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
    std::string b = sg1; int c = 0; for (int i = 0; i < 32; i++) { int v = (uint8_t)b[32 + i] + L[i] + c; b[32 + i] = static_cast<char>(v & 255); c = v >> 8; }
    check(!vfy(pk1, "", b), "non-canonical S (S+L) is rejected");
  }
  { uint8_t seed[32], pk[32], sg[64]; std::string sd = hx("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
    memcpy(seed, sd.data(), 32); C::ed25519_pubkey(seed, pk);
    check(C::hex_encode(pk, 32) == "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "pubkey derivation matches RFC vector 2");
    const std::string m = "hello pnr"; C::ed25519_sign(seed, reinterpret_cast<const uint8_t*>(m.data()), m.size(), sg);
    check(C::ed25519_verify(pk, reinterpret_cast<const uint8_t*>(m.data()), m.size(), sg), "sign then verify round-trips"); }

  // ---- store policy ----
  const std::string dir = "/tmp/pnr_sig_" + std::to_string(::getpid());
  fs::remove_all(dir); fs::create_directories(dir);
  Registry reg(dir); reg.load();
  std::string out;
  cmd_source_add(reg, "demo", "https://games.example.invalid/index.json", "mine", false, &out);
  uint8_t seed[32]; { std::string sd = hx("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"); memcpy(seed, sd.data(), 32); }
  uint8_t pk[32]; C::ed25519_pubkey(seed, pk);
  const std::string pkhex = C::hex_encode(pk, 32);
  Package p; p.id = "alpha"; p.version = "1"; p.sha256 = std::string(64, 'a'); p.size = 1000;
  std::string mode;
  check(check_signature(*reg.find("demo"), p, &mode) && mode == "unsigned (hash only)", "no pinned key: hash-only, labelled unsigned");
  check(!reg.set_pubkey("demo", "zz"), "malformed pinned key is refused");
  check(reg.set_pubkey("demo", pkhex).ok, "pin key");
  { Registry r2(dir); r2.load(); check(r2.find("demo") && r2.find("demo")->pubkey == pkhex, "pinned key persists"); }
  pnr::Status s = check_signature(*reg.find("demo"), p, &mode);
  check(!s && s.error.code == kErrSignature, "pinned key + unsigned package is refused");
  const std::string msg = signed_message("demo", p); uint8_t sg[64];
  C::ed25519_sign(seed, reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), sg);
  p.signature = C::hex_encode(sg, 64);
  check(check_signature(*reg.find("demo"), p, &mode) && mode == "signature verified (pinned key)", "valid signature accepted");
  { Package t = p; t.sha256 = std::string(64, 'b'); check(!check_signature(*reg.find("demo"), t, &mode), "modified hash is refused"); }
  { Package t = p; t.size = 1001; check(!check_signature(*reg.find("demo"), t, &mode), "modified size is refused"); }
  { Package t = p; t.version = "2"; check(!check_signature(*reg.find("demo"), t, &mode), "modified version is refused"); }
  { Package t = p; t.id = "beta"; check(!check_signature(*reg.find("demo"), t, &mode), "signature does not transfer to another package id"); }
  { Source other = *reg.find("demo"); other.id = "other"; check(!check_signature(other, p, &mode), "signature does not transfer to another source id"); }
  // through the install gate: a pinned source's unsigned package never reaches the hash check
  const std::string idx = "{\"format\":\"pnr-store-index-1\",\"name\":\"D\",\"rights\":\"mine\",\"packages\":[{\"id\":\"alpha\",\"name\":\"N\",\"version\":\"1\",\"license\":\"MIT\",\"kind\":\"pnr-package\",\"size\":1000,\"sha256\":\"" + std::string(64, 'a') + "\",\"url\":\"https://games.example.invalid/f/a.pnrgame\"}]}";
  reg.save_index_text("demo", idx);
  { Queue q(reg, dir); std::string path, sha; pnr::Status r = q.prepare_install("demo", "alpha", &path, &sha);
    check(!r && r.error.code == kErrSignature, "prepare_install refuses an unsigned package from a pinned source"); }
  { cmd_source_pin(reg, "demo", "none", false, &out); Queue q(reg, dir); std::string path, sha; pnr::Status r = q.prepare_install("demo", "alpha", &path, &sha);
    check(!r && r.error.code != kErrSignature, "unpinned source falls through to the normal not-verified check"); }
  { std::string bad = idx; bad.replace(bad.find("\"url\""), 0, "\"signature\":\"xyz\","); reg.save_index_text("demo", bad); Index ix;
    check(!reg.load_index("demo", &ix), "index with a malformed signature is rejected"); }
  fs::remove_all(dir);
}
