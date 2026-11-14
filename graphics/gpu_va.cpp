// PNR Graphics - GPU virtual address map (Core slice C4c), GPL-2.0.
#include "graphics/gpu_va.hpp"

#include <cstdio>
#include <cstring>

namespace pnr::graphics {

std::string GpuAddressSpace::insert(std::uint64_t va, Region r) {
  if (r.size == 0) return "mapping has zero size";
  if (va + r.size < va) return "mapping wraps past the end of the address space";
  auto next = regions_.lower_bound(va);
  if (next != regions_.end() && next->first < va + r.size) return "mapping overlaps '" + next->second.name + "'";
  if (next != regions_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second.size > va) return "mapping overlaps '" + prev->second.name + "'";
  }
  regions_.emplace(va, std::move(r));
  return {};
}

std::string GpuAddressSpace::map_external(std::uint64_t va, void* host, std::size_t size, const std::string& name) {
  if (!host) return "host pointer is null";
  Region r{size, static_cast<std::uint8_t*>(host), name, {}};
  return insert(va, std::move(r));
}

std::string GpuAddressSpace::map_copy(std::uint64_t va, const void* data, std::size_t size, const std::string& name) {
  if (!data && size) return "data pointer is null";
  Region r{size, nullptr, name, std::vector<std::uint8_t>(size)};
  if (size) std::memcpy(r.owned.data(), data, size);
  std::uint8_t* p = r.owned.data();  // vector buffer address survives the move into the map
  r.host = p;
  return insert(va, std::move(r));
}

bool GpuAddressSpace::unmap(std::uint64_t va) { return regions_.erase(va) != 0; }

const GpuAddressSpace::Region* GpuAddressSpace::find(std::uint64_t va, std::size_t bytes, std::uint64_t* off) const {
  auto it = regions_.upper_bound(va);
  char buf[96];
  if (it == regions_.begin()) {
    std::snprintf(buf, sizeof buf, "address 0x%llx is not mapped", (unsigned long long)va);
    err_ = buf;
    return nullptr;
  }
  --it;
  const std::uint64_t o = va - it->first;
  if (o >= it->second.size) {
    std::snprintf(buf, sizeof buf, "address 0x%llx is not mapped", (unsigned long long)va);
    err_ = buf;
    return nullptr;
  }
  if (bytes > it->second.size - o) {
    std::snprintf(buf, sizeof buf, "range at 0x%llx runs past the end of '", (unsigned long long)va);
    err_ = std::string(buf) + it->second.name + "'";
    return nullptr;
  }
  *off = o;
  return &it->second;
}

const std::uint8_t* GpuAddressSpace::resolve(std::uint64_t va, std::size_t bytes) const {
  std::uint64_t off = 0;
  const Region* r = find(va, bytes, &off);
  return r ? r->host + off : nullptr;
}
std::uint8_t* GpuAddressSpace::resolve_mut(std::uint64_t va, std::size_t bytes) {
  return const_cast<std::uint8_t*>(resolve(va, bytes));
}
bool GpuAddressSpace::read(std::uint64_t va, void* out, std::size_t bytes) const {
  const std::uint8_t* p = resolve(va, bytes);
  if (!p) return false;
  if (bytes) std::memcpy(out, p, bytes);
  return true;
}
bool GpuAddressSpace::write(std::uint64_t va, const void* in, std::size_t bytes) {
  std::uint8_t* p = resolve_mut(va, bytes);
  if (!p) return false;
  if (bytes) std::memcpy(p, in, bytes);
  return true;
}

std::function<const std::uint32_t*(std::uint64_t, std::size_t)> GpuAddressSpace::pm4_resolver() const {
  return [this](std::uint64_t va, std::size_t dwords) -> const std::uint32_t* {
    if (va & 3) { err_ = "indirect buffer address is not 4-byte aligned"; return nullptr; }
    const std::uint8_t* p = resolve(va, dwords * 4);
    if (!p) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(p) & 3) { err_ = "host memory is not 4-byte aligned"; return nullptr; }
    return reinterpret_cast<const std::uint32_t*>(p);
  };
}

std::string GpuAddressSpace::describe() const {
  std::string s;
  char buf[160];
  for (const auto& kv : regions_) {
    std::snprintf(buf, sizeof buf, "  0x%012llx..0x%012llx %s\n", (unsigned long long)kv.first,
                  (unsigned long long)(kv.first + kv.second.size), kv.second.name.c_str());
    s += buf;
  }
  return s.empty() ? "  (no mappings)\n" : s;
}

}  // namespace pnr::graphics
