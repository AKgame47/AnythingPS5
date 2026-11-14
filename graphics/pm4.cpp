#include "graphics/pm4.hpp"

#include <cstdio>

namespace pnr::graphics::pm4 {

namespace {
std::uint32_t hdr3(std::uint8_t op, std::size_t body) { return (3u << 30) | (static_cast<std::uint32_t>(op) << 8) | (static_cast<std::uint32_t>(body - 1) & 0x3fff) << 16; }
std::string hex(std::uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v)); return b; }
}  // namespace

const char* opcode_name(std::uint8_t op) {
  switch (op) {
    case NOP: return "NOP"; case SET_BASE: return "SET_BASE"; case CLEAR_STATE: return "CLEAR_STATE";
    case DISPATCH_DIRECT: return "DISPATCH_DIRECT"; case DRAW_INDIRECT: return "DRAW_INDIRECT";
    case DRAW_INDEX_INDIRECT: return "DRAW_INDEX_INDIRECT"; case INDEX_BASE: return "INDEX_BASE";
    case DRAW_INDEX_2: return "DRAW_INDEX_2"; case CONTEXT_CONTROL: return "CONTEXT_CONTROL";
    case INDEX_TYPE: return "INDEX_TYPE"; case DRAW_INDEX_AUTO: return "DRAW_INDEX_AUTO";
    case NUM_INSTANCES: return "NUM_INSTANCES"; case INDIRECT_BUFFER_CONST: return "INDIRECT_BUFFER_CONST";
    case DRAW_INDEX_OFFSET_2: return "DRAW_INDEX_OFFSET_2"; case WRITE_DATA: return "WRITE_DATA";
    case WAIT_REG_MEM: return "WAIT_REG_MEM"; case INDIRECT_BUFFER: return "INDIRECT_BUFFER";
    case PFP_SYNC_ME: return "PFP_SYNC_ME"; case EVENT_WRITE: return "EVENT_WRITE"; case RELEASE_MEM: return "RELEASE_MEM";
    case DMA_DATA: return "DMA_DATA"; case ACQUIRE_MEM: return "ACQUIRE_MEM"; case SET_CONFIG_REG: return "SET_CONFIG_REG";
    case SET_CONTEXT_REG: return "SET_CONTEXT_REG"; case SET_SH_REG: return "SET_SH_REG"; case SET_UCONFIG_REG: return "SET_UCONFIG_REG";
    default: return "?";
  }
}

std::vector<Packet> decode(const std::uint32_t* dw, std::size_t count, std::vector<Issue>* issues) {
  std::vector<Packet> out;
  auto issue = [&](std::size_t at, const std::string& w) { if (issues) issues->push_back({at, w}); };
  std::size_t i = 0;
  while (i < count) {
    const std::uint32_t h = dw[i];
    const std::uint32_t type = h >> 30, n = ((h >> 16) & 0x3fff) + 1;
    Packet p; p.type = type; p.offset = i;
    if (type == 2) { out.push_back(p); ++i; continue; }  // filler, no body
    if (type == 1) { issue(i, "type 1 packet is reserved"); break; }
    if (n > count - i - 1) { issue(i, "packet body runs past the end of the buffer (" + std::to_string(n) + " dwords declared, " + std::to_string(count - i - 1) + " left)"); break; }
    if (type == 0) p.reg = h & 0xffff;
    else { p.opcode = static_cast<std::uint8_t>((h >> 8) & 0xff); p.compute = (h >> 1) & 1; p.predicate = h & 1; }
    p.body.assign(dw + i + 1, dw + i + 1 + n);
    out.push_back(std::move(p));
    i += 1 + n;
  }
  return out;
}

namespace {
bool window(std::uint8_t op, std::uint32_t* start, std::uint32_t* end) {
  switch (op) {
    case SET_CONFIG_REG: *start = kConfigStart; *end = kConfigEnd; return true;
    case SET_CONTEXT_REG: *start = kContextStart; *end = kContextEnd; return true;
    case SET_SH_REG: *start = kShStart; *end = kShEnd; return true;
    case SET_UCONFIG_REG: *start = kUconfigStart; *end = kUconfigEnd; return true;
    default: return false;
  }
}

struct Machine {
  Summary& s;
  const Resolver& res;
  std::uint32_t instances = 1, index_bytes = 4;
  std::uint64_t index_base = 0;

  void run(const std::uint32_t* dw, std::size_t n, int depth) {
    s.dwords += n;
    std::vector<Issue> iss;
    const auto pk = decode(dw, n, &iss);
    for (auto& i : iss) { i.offset += 0; s.issues.push_back(i); }
    for (const auto& p : pk) {
      ++s.packets;
      if (p.type != 3) continue;
      ++s.by_opcode[p.opcode];
      step(p, depth);
    }
  }

  void step(const Packet& p, int depth) {
    const auto& b = p.body;
    std::uint32_t ws = 0, we = 0;
    if (window(p.opcode, &ws, &we)) {
      if (b.size() < 2) { s.issues.push_back({p.offset, std::string(opcode_name(p.opcode)) + " has no value"}); return; }
      const std::uint32_t first = ws + b[0];
      for (std::size_t k = 1; k < b.size(); ++k) {
        const std::uint32_t r = first + static_cast<std::uint32_t>(k - 1);
        if (r >= we) { s.issues.push_back({p.offset, "register " + hex(r) + " is outside the " + opcode_name(p.opcode) + " window"}); return; }
        s.regs[r] = b[k]; ++s.register_writes;
      }
      return;
    }
    switch (p.opcode) {
      case NOP: case CONTEXT_CONTROL: case PFP_SYNC_ME: case ACQUIRE_MEM: case EVENT_WRITE: break;
      case CLEAR_STATE: { std::map<std::uint32_t, std::uint32_t> keep; for (auto& kv : s.regs) if (kv.first < kContextStart || kv.first >= kContextEnd) keep.insert(kv); s.regs.swap(keep); break; }
      case INDEX_TYPE: if (!b.empty()) index_bytes = (b[0] & 3) == 0 ? 2 : 4; break;
      case NUM_INSTANCES: if (!b.empty()) instances = b[0]; break;
      case DRAW_INDEX_AUTO:
        if (b.size() < 2) { s.issues.push_back({p.offset, "DRAW_INDEX_AUTO is too short"}); break; }
        { DrawCall d; d.kind = DrawKind::Auto; d.count = b[0]; d.initiator = b[1]; d.instances = instances; d.packet_offset = p.offset; d.state = s.regs; s.draws.push_back(std::move(d)); }
        break;
      case DRAW_INDEX_2:
        if (b.size() < 5) { s.issues.push_back({p.offset, "DRAW_INDEX_2 is too short"}); break; }
        { DrawCall d; d.kind = DrawKind::Indexed; d.count = b[3]; d.initiator = b[4]; d.instances = instances; d.index_size_bytes = index_bytes;
          d.index_va = (static_cast<std::uint64_t>(b[2]) << 32) | b[1]; d.packet_offset = p.offset; d.state = s.regs; s.draws.push_back(std::move(d)); }
        break;
      case DISPATCH_DIRECT:
        if (b.size() < 4) { s.issues.push_back({p.offset, "DISPATCH_DIRECT is too short"}); break; }
        s.dispatches.push_back({b[0], b[1], b[2], b[3], p.offset});
        break;
      case WRITE_DATA:
        // body: control, addr_lo, addr_hi, data...
        if (b.size() < 4) { s.issues.push_back({p.offset, "WRITE_DATA is too short"}); break; }
        s.writes.push_back({(static_cast<std::uint64_t>(b[2]) << 32) | b[1], b[3], p.offset, "WRITE_DATA"});
        break;
      case INDIRECT_BUFFER: case INDIRECT_BUFFER_CONST: {
        if (b.size() < 3) { s.issues.push_back({p.offset, "INDIRECT_BUFFER is too short"}); break; }
        const std::uint64_t va = (static_cast<std::uint64_t>(b[1]) << 32) | b[0];
        const std::size_t dwords = b[2] & 0xfffff;
        const std::uint32_t* host = (res && depth < 4) ? res(va, dwords) : nullptr;
        if (!host) { ++s.indirect_unresolved; break; }
        run(host, dwords, depth + 1);
        break;
      }
      default: ++s.unhandled[p.opcode]; break;
    }
  }
};
}  // namespace

Summary interpret(const std::uint32_t* dw, std::size_t count, const Resolver& resolver) {
  Summary s;
  Machine m{s, resolver};
  m.run(dw, count, 0);
  return s;
}

std::string describe(const Packet& p) {
  char b[160];
  if (p.type == 2) { std::snprintf(b, sizeof b, "%6zu  type2 filler", p.offset); return b; }
  if (p.type == 0) { std::snprintf(b, sizeof b, "%6zu  type0 reg=0x%x n=%zu", p.offset, p.reg, p.body.size()); return b; }
  std::snprintf(b, sizeof b, "%6zu  %-20s op=0x%02x body=%zu%s%s", p.offset, opcode_name(p.opcode), p.opcode, p.body.size(), p.compute ? " compute" : "", p.predicate ? " predicated" : "");
  return b;
}

std::string report(const Summary& s) {
  std::string o;
  char b[200];
  std::snprintf(b, sizeof b, "%zu dwords, %zu packets, %zu register writes, %zu draws, %zu dispatches, %zu memory writes\n", s.dwords, s.packets, s.register_writes, s.draws.size(), s.dispatches.size(), s.writes.size());
  o += b;
  for (const auto& kv : s.by_opcode) { std::snprintf(b, sizeof b, "  %-22s x%zu\n", opcode_name(kv.first), kv.second); o += b; }
  for (const auto& d : s.draws) {
    std::snprintf(b, sizeof b, "draw @%zu: %s count=%u instances=%u%s\n", d.packet_offset, d.kind == DrawKind::Auto ? "auto" : "indexed", d.count, d.instances, d.kind == DrawKind::Indexed ? (" index_size=" + std::to_string(d.index_size_bytes) + " va=" + hex(d.index_va)).c_str() : "");
    o += b;
  }
  if (s.indirect_unresolved) { std::snprintf(b, sizeof b, "%zu indirect buffer(s) NOT followed (no address resolver)\n", s.indirect_unresolved); o += b; }
  for (const auto& kv : s.unhandled) { std::snprintf(b, sizeof b, "UNHANDLED opcode 0x%02x (%s) x%zu: decoded, no state meaning in PNR yet\n", kv.first, opcode_name(kv.first), kv.second); o += b; }
  for (const auto& i : s.issues) { std::snprintf(b, sizeof b, "ISSUE at dword %zu: ", i.offset); o += b; o += i.what; o += "\n"; }
  return o;
}

Builder& Builder::nop(std::uint32_t n) { dw_.push_back(hdr3(NOP, n)); for (std::uint32_t i = 0; i < n; ++i) dw_.push_back(0); return *this; }
Builder& Builder::packet3(std::uint8_t op, const std::vector<std::uint32_t>& body) { dw_.push_back(hdr3(op, body.size())); dw_.insert(dw_.end(), body.begin(), body.end()); return *this; }
Builder& Builder::set_reg(std::uint8_t op, std::uint32_t abs_reg, const std::vector<std::uint32_t>& v) {
  std::uint32_t s = 0, e = 0; window(op, &s, &e);
  std::vector<std::uint32_t> body{abs_reg - s}; body.insert(body.end(), v.begin(), v.end());
  return packet3(op, body);
}
Builder& Builder::index_type(std::uint32_t t) { return packet3(INDEX_TYPE, {t}); }
Builder& Builder::num_instances(std::uint32_t n) { return packet3(NUM_INSTANCES, {n}); }
Builder& Builder::draw_auto(std::uint32_t c) { return packet3(DRAW_INDEX_AUTO, {c, 2}); }
Builder& Builder::draw_indexed(std::uint64_t va, std::uint32_t max_size, std::uint32_t c) { return packet3(DRAW_INDEX_2, {max_size, static_cast<std::uint32_t>(va), static_cast<std::uint32_t>(va >> 32), c, 0}); }
Builder& Builder::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return packet3(DISPATCH_DIRECT, {x, y, z, 1}); }
Builder& Builder::write_data(std::uint64_t va, std::uint32_t v) { return packet3(WRITE_DATA, {0x100, static_cast<std::uint32_t>(va), static_cast<std::uint32_t>(va >> 32), v}); }
Builder& Builder::indirect(std::uint64_t va, std::uint32_t n) { return packet3(INDIRECT_BUFFER, {static_cast<std::uint32_t>(va), static_cast<std::uint32_t>(va >> 32), n}); }
Builder& Builder::raw(const std::vector<std::uint32_t>& d) { dw_.insert(dw_.end(), d.begin(), d.end()); return *this; }

}  // namespace pnr::graphics::pm4
