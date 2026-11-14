// PNR Graphics - PM4 command buffer decoder and state tracker (Core slice C4, part 1), GPL-2.0.
// PM4 is the packet format AMD GPUs read; PlayStation GPU command buffers are built from it too.
// This module only DECODES and TRACKS state. It does not talk to Vulkan yet and it does not run
// shaders. It is headless and has no GPU dependency.
//
// Verified against a public source: packet header layout, opcode numbers and register window
// bases come from the Linux kernel header drivers/gpu/drm/amd/amdgpu/soc15d.h (facts only, no code).
// NOT verified against a PlayStation: the field layout of draw packet bodies (taken from public
// driver usage), anything Sony-specific, and the meaning of any register number. Unknown opcodes
// are counted and reported, never guessed. Tested on SYNTHETIC command buffers only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pnr::graphics::pm4 {

enum Opcode : std::uint8_t {
  NOP = 0x10, SET_BASE = 0x11, CLEAR_STATE = 0x12, DISPATCH_DIRECT = 0x15, DRAW_INDIRECT = 0x24,
  DRAW_INDEX_INDIRECT = 0x25, INDEX_BASE = 0x26, DRAW_INDEX_2 = 0x27, CONTEXT_CONTROL = 0x28,
  INDEX_TYPE = 0x2A, DRAW_INDEX_AUTO = 0x2D, NUM_INSTANCES = 0x2F, INDIRECT_BUFFER_CONST = 0x33,
  DRAW_INDEX_OFFSET_2 = 0x35, WRITE_DATA = 0x37, WAIT_REG_MEM = 0x3C, INDIRECT_BUFFER = 0x3F,
  PFP_SYNC_ME = 0x42, EVENT_WRITE = 0x46, RELEASE_MEM = 0x49, DMA_DATA = 0x50, ACQUIRE_MEM = 0x58,
  SET_CONFIG_REG = 0x68, SET_CONTEXT_REG = 0x69, SET_SH_REG = 0x76, SET_UCONFIG_REG = 0x79,
};

constexpr std::uint32_t kConfigStart = 0x2000, kConfigEnd = 0x2c00, kContextStart = 0xa000, kContextEnd = 0xa400,
                        kShStart = 0x2c00, kShEnd = 0x3000, kUconfigStart = 0xc000, kUconfigEnd = 0xc400;

struct Packet {
  std::uint32_t type = 0;       // 0, 2 or 3 (1 is reserved)
  std::uint8_t opcode = 0;      // type 3 only
  std::uint32_t reg = 0;        // type 0 only
  bool predicate = false, compute = false;
  std::size_t offset = 0;       // dword index of the header
  std::vector<std::uint32_t> body;
};

struct Issue { std::size_t offset; std::string what; };

// Splits a dword stream into packets. Stops at the first malformed packet and records why.
std::vector<Packet> decode(const std::uint32_t* dw, std::size_t count, std::vector<Issue>* issues);
const char* opcode_name(std::uint8_t op);  // "?" when unknown

enum class DrawKind { Auto, Indexed };
struct DrawCall {
  DrawKind kind = DrawKind::Auto;
  std::uint32_t count = 0, instances = 1, index_size_bytes = 0, initiator = 0;
  std::uint64_t index_va = 0;
  std::size_t packet_offset = 0;
  std::map<std::uint32_t, std::uint32_t> state;  // register file at the time of the draw
};
struct Dispatch { std::uint32_t x = 0, y = 0, z = 0, initiator = 0; std::size_t packet_offset = 0; };
struct MemWrite { std::uint64_t va = 0; std::uint64_t data = 0; std::size_t packet_offset = 0; std::string source; };

struct Summary {
  std::size_t dwords = 0, packets = 0, register_writes = 0, indirect_unresolved = 0;
  std::map<std::uint8_t, std::size_t> by_opcode;
  std::map<std::uint8_t, std::size_t> unhandled;  // opcodes with no state meaning here
  std::vector<DrawCall> draws;
  std::vector<Dispatch> dispatches;
  std::vector<MemWrite> writes;
  std::map<std::uint32_t, std::uint32_t> regs;    // final register file
  std::vector<Issue> issues;
};

// Maps a GPU virtual address to host memory holding `dwords` dwords, or null. Used to follow
// INDIRECT_BUFFER. Without a resolver, indirect buffers are counted and not followed.
using Resolver = std::function<const std::uint32_t*(std::uint64_t va, std::size_t dwords)>;

// Runs the stream through the state tracker. Indirect buffers nest at most 4 deep.
Summary interpret(const std::uint32_t* dw, std::size_t count, const Resolver& resolver = nullptr);

// One line per packet, for `pnr gnm-decode`.
std::string describe(const Packet& p);
std::string report(const Summary& s);

// Builds SYNTHETIC command buffers for tests and demos.
class Builder {
 public:
  Builder& nop(std::uint32_t n = 1);
  Builder& set_reg(std::uint8_t opcode, std::uint32_t abs_reg, const std::vector<std::uint32_t>& values);
  Builder& set_context(std::uint32_t abs_reg, std::uint32_t v) { return set_reg(SET_CONTEXT_REG, abs_reg, {v}); }
  Builder& set_sh(std::uint32_t abs_reg, std::uint32_t v) { return set_reg(SET_SH_REG, abs_reg, {v}); }
  Builder& set_uconfig(std::uint32_t abs_reg, std::uint32_t v) { return set_reg(SET_UCONFIG_REG, abs_reg, {v}); }
  Builder& index_type(std::uint32_t t);
  Builder& num_instances(std::uint32_t n);
  Builder& draw_auto(std::uint32_t count);
  Builder& draw_indexed(std::uint64_t va, std::uint32_t max_size, std::uint32_t count);
  Builder& dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z);
  Builder& write_data(std::uint64_t va, std::uint32_t value);
  Builder& indirect(std::uint64_t va, std::uint32_t dwords);
  Builder& raw(const std::vector<std::uint32_t>& dw);
  Builder& packet3(std::uint8_t op, const std::vector<std::uint32_t>& body);
  const std::vector<std::uint32_t>& dwords() const { return dw_; }
 private:
  std::vector<std::uint32_t> dw_;
};

}  // namespace pnr::graphics::pm4
