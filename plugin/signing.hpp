// PNR plugin system - Ed25519 plugin signatures (slice P3), GPL-2.0.
// A signed plugin has a file "pnr-plugin.sig": {"key":"<64 hex pubkey>","signature":"<128 hex>"}.
// The signed message is "pnr-plugin-v1\n" then, for every file except pnr-plugin.sig in path order, "<path>\n<sha512 hex>\n".
// So changing, adding or removing any file breaks the signature. A signature only proves the holder of a key you PINNED made
// this exact tree. It says nothing about whether the code is safe, and an unpinned key proves nothing. Implemented, Tested.
#pragma once
#include <string>
#include <vector>

namespace pnr::plugin {

inline constexpr const char* kSigFile = "pnr-plugin.sig";
struct PinnedKey { std::string name, pubkey_hex; };

// Message that gets signed. False + *err if a file cannot be read.
bool signing_message(const std::string& dir, std::string* msg, std::string* err);
// Author side: writes <dir>/pnr-plugin.sig using a 32-byte seed (hex). Returns the public key hex via *pub.
bool sign_dir(const std::string& dir, const std::string& seed_hex, std::string* pub, std::string* err);

enum class SigState { Unsigned, Valid, Invalid, UntrustedKey };
struct SigCheck { SigState state = SigState::Unsigned; std::string key_name, key_hex, detail; };
SigCheck check_signature(const std::string& dir, const std::vector<PinnedKey>& pinned);

}  // namespace pnr::plugin
