// PNR Graphics - GPU virtual address map (Core slice C4c), GPL-2.0.
// Maps guest GPU virtual address ranges to host memory so PM4 indirect buffers, shader code, vertex
// and index data can be read by address. This is a plain range table: no paging, no page tables,
// no caching or coherency model. Implemented, Tested on synthetic data.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pnr::graphics {

class GpuAddressSpace {
 public:
  // Map [va, va+size) to caller-owned host memory (must outlive the map). Returns an empty string on
  // success, otherwise a plain-words error (zero size, wrap-around, overlap, null host pointer).
  std::string map_external(std::uint64_t va, void* host, std::size_t size, const std::string& name);
  // Map and copy: the address space owns a copy of the bytes.
  std::string map_copy(std::uint64_t va, const void* data, std::size_t size, const std::string& name);
  // Remove the mapping that starts exactly at va. False if none.
  bool unmap(std::uint64_t va);

  // Pointer to `bytes` bytes at va, only if the whole range lies inside ONE mapping. Else nullptr and
  // last_error() says why (unmapped, or crosses the end of the mapping).
  const std::uint8_t* resolve(std::uint64_t va, std::size_t bytes) const;
  std::uint8_t* resolve_mut(std::uint64_t va, std::size_t bytes);
  // Copy out, same rules. False on failure.
  bool read(std::uint64_t va, void* out, std::size_t bytes) const;
  bool write(std::uint64_t va, const void* in, std::size_t bytes);
  // Adapter for pm4::interpret: requires 4-byte alignment.
  std::function<const std::uint32_t*(std::uint64_t, std::size_t)> pm4_resolver() const;

  std::size_t region_count() const { return regions_.size(); }
  const std::string& last_error() const { return err_; }
  // One line per region, for diagnostics.
  std::string describe() const;

 private:
  struct Region { std::uint64_t size; std::uint8_t* host; std::string name; std::vector<std::uint8_t> owned; };
  const Region* find(std::uint64_t va, std::size_t bytes, std::uint64_t* off) const;
  std::string insert(std::uint64_t va, Region r);
  std::map<std::uint64_t, Region> regions_;
  mutable std::string err_;
};

}  // namespace pnr::graphics
