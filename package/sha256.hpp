// SHA-256 (FIPS 180-4) - self-contained implementation for package
// integrity checks. Tested against published test vectors.
// Status: Implemented, Tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pnr::pkg {

class Sha256 {
public:
  Sha256();
  void update(const void* data, size_t len);
  std::vector<uint8_t> finish();       // 32 raw bytes; consumes the state
  std::string finish_hex();            // 64 lowercase hex chars

private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

std::string sha256_hex(const void* data, size_t len);
std::string sha256_hex(const std::string& s);

}  // namespace pnr::pkg
