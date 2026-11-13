// PNR Ed25519 signature VERIFY (RFC 8032, pure Ed25519), slice B1. GPL-2.0.
// Verify is what the store uses. Sign/pubkey exist for tests and for source
// publishers' tooling; PNR itself never holds or generates signing keys. Written for PNR from the public
// RFC 8032 algorithm using the well-known 16x16-bit-limb field arithmetic
// (the technique of the public-domain TweetNaCl). Constants are derived at
// run time from their definitions. Tested against RFC 8032 section 7.1 vectors.
// Not constant-time: it only handles public data (keys, messages, signatures).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace pnr::crypto {
// SHA-512 (FIPS 180-4), 64-byte digest.
void sha512(const uint8_t* data, size_t len, uint8_t out[64]);
// True only if sig (64 bytes) is a valid Ed25519 signature of msg under pk (32 bytes).
// Rejects non-canonical S (S >= L) as RFC 8032 requires.
bool ed25519_verify(const uint8_t pk[32], const uint8_t* msg, size_t len, const uint8_t sig[64]);
void ed25519_pubkey(const uint8_t seed[32], uint8_t pk[32]);
void ed25519_sign(const uint8_t seed[32], const uint8_t* msg, size_t len, uint8_t sig[64]);
// Hex helpers; hex_decode returns false on odd length or non-hex.
bool hex_decode(const std::string& hex, std::string* out);
std::string hex_encode(const uint8_t* d, size_t n);
}  // namespace pnr::crypto
