// PNR plugin system - Ed25519 plugin signatures (slice P3), GPL-2.0. See signing.hpp.
#include "plugin/signing.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "crypto/ed25519.hpp"
#include "psn/json.hpp"

namespace fs = std::filesystem;
namespace pnr::plugin {

bool signing_message(const std::string& dir, std::string* msg, std::string* err) {
  std::vector<std::string> rels;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) { std::string r = fs::relative(it->path(), dir, ec).generic_string(); if (r != kSigFile) rels.push_back(r); }
  std::sort(rels.begin(), rels.end());
  *msg = "pnr-plugin-v1\n";
  for (auto& r : rels) {
    std::ifstream in(fs::path(dir) / r, std::ios::binary);
    if (!in) { *err = "cannot read " + r; return false; }
    std::stringstream ss; ss << in.rdbuf(); std::string d = ss.str();
    uint8_t h[64]; crypto::sha512((const uint8_t*)d.data(), d.size(), h);
    *msg += r + "\n" + crypto::hex_encode(h, 64) + "\n";
  }
  return true;
}

bool sign_dir(const std::string& dir, const std::string& seed_hex, std::string* pub, std::string* err) {
  std::string seed;
  if (!crypto::hex_decode(seed_hex, &seed) || seed.size() != 32) { *err = "seed must be 64 hex characters (32 bytes)"; return false; }
  std::string msg;
  if (!signing_message(dir, &msg, err)) return false;
  uint8_t pk[32], sig[64];
  crypto::ed25519_pubkey((const uint8_t*)seed.data(), pk);
  crypto::ed25519_sign((const uint8_t*)seed.data(), (const uint8_t*)msg.data(), msg.size(), sig);
  *pub = crypto::hex_encode(pk, 32);
  std::ofstream(fs::path(dir) / kSigFile, std::ios::trunc) << "{\"key\":\"" << *pub << "\",\"signature\":\"" << crypto::hex_encode(sig, 64) << "\"}\n";
  return true;
}

SigCheck check_signature(const std::string& dir, const std::vector<PinnedKey>& pinned) {
  SigCheck c;
  std::ifstream in(fs::path(dir) / kSigFile);
  if (!in) return c;  // Unsigned
  std::stringstream ss; ss << in.rdbuf();
  psn::Json j; std::string je;
  if (ss.str().size() > 4096 || !psn::json_parse(ss.str(), &j, &je) || !j.is(psn::Json::T::Obj)) { c.state = SigState::Invalid; c.detail = "pnr-plugin.sig is not valid JSON"; return c; }
  const psn::Json* k = j.get("key"); const psn::Json* s = j.get("signature");
  std::string kb, sb;
  if (!k || !s || !k->is(psn::Json::T::Str) || !s->is(psn::Json::T::Str) || !crypto::hex_decode(k->s, &kb) || kb.size() != 32 || !crypto::hex_decode(s->s, &sb) || sb.size() != 64) {
    c.state = SigState::Invalid; c.detail = "pnr-plugin.sig has a malformed key or signature"; return c;
  }
  c.key_hex = k->s;
  std::string msg, err;
  if (!signing_message(dir, &msg, &err)) { c.state = SigState::Invalid; c.detail = err; return c; }
  if (!crypto::ed25519_verify((const uint8_t*)kb.data(), (const uint8_t*)msg.data(), msg.size(), (const uint8_t*)sb.data())) {
    c.state = SigState::Invalid; c.detail = "signature does not match the files (changed, added or removed since signing)"; return c;
  }
  for (auto& p : pinned) {
    std::string pb; if (crypto::hex_decode(p.pubkey_hex, &pb) && pb == kb) { c.state = SigState::Valid; c.key_name = p.name; return c; }
  }
  c.state = SigState::UntrustedKey; c.detail = "signature is valid but the key is not pinned, so it proves nothing about who made it";
  return c;
}

}  // namespace pnr::plugin
