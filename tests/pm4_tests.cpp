// Core slice C4 (part 1) tests: PM4 decode and state tracking on SYNTHETIC command buffers.
#include <cstdio>
#include <string>

#include "graphics/pm4.hpp"

void pnr_pm4_tests(int& pass, int& fail) {
  using namespace pnr::graphics::pm4;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };

  // Header layout (kernel soc15d.h): PACKET3(op, n) = 3<<30 | op<<8 | n<<16, body = n+1 dwords.
  Builder b;
  b.nop(3);
  std::vector<Issue> iss;
  auto pk = decode(b.dwords().data(), b.dwords().size(), &iss);
  check(pk.size() == 1 && pk[0].type == 3 && pk[0].opcode == NOP && pk[0].body.size() == 3 && iss.empty(), "decode: NOP with 3 body dwords");
  check(b.dwords()[0] == ((3u << 30) | (0x10u << 8) | (2u << 16)), "decode: header bits match the PACKET3 layout");

  // Type 0, type 2 and truncation.
  { std::vector<std::uint32_t> d = {(0u << 30) | (1u << 16) | 0x1234, 7, 8, 0x80000000u, (3u << 30) | (0x10u << 8)};
    d.push_back(0);
    iss.clear(); pk = decode(d.data(), d.size(), &iss);
    check(pk.size() == 3 && pk[0].type == 0 && pk[0].reg == 0x1234 && pk[0].body.size() == 2 && pk[1].type == 2 && pk[2].opcode == NOP, "decode: type 0 with register, type 2 filler, type 3");
    std::vector<std::uint32_t> t = {(3u << 30) | (0x10u << 8) | (5u << 16), 1, 2};
    iss.clear(); pk = decode(t.data(), t.size(), &iss);
    check(pk.empty() && iss.size() == 1 && iss[0].what.find("past the end") != std::string::npos, "decode: a truncated packet is reported, not read past");
    std::vector<std::uint32_t> r = {(1u << 30)};
    iss.clear(); pk = decode(r.data(), r.size(), &iss);
    check(pk.empty() && iss.size() == 1 && iss[0].what.find("reserved") != std::string::npos, "decode: reserved type 1 is refused");
  }

  // Register windows and state at the time of a draw.
  Builder g;
  g.set_context(0xa010, 0x11).set_sh(0x2c10, 0x22).set_uconfig(0xc242, 4).set_reg(SET_CONTEXT_REG, 0xa100, {1, 2, 3}).num_instances(5).draw_auto(36);
  auto s = interpret(g.dwords().data(), g.dwords().size());
  check(s.draws.size() == 1 && s.draws[0].count == 36 && s.draws[0].instances == 5 && s.draws[0].kind == DrawKind::Auto, "interpret: DRAW_INDEX_AUTO captured with count and instances");
  check(s.regs[0xa010] == 0x11 && s.regs[0x2c10] == 0x22 && s.regs[0xc242] == 4 && s.regs[0xa100] == 1 && s.regs[0xa102] == 3 && s.register_writes == 6, "interpret: SET_*_REG write the right absolute registers");
  check(s.draws[0].state.at(0xa010) == 0x11, "interpret: a draw carries the register state at that moment");

  // State at each draw differs.
  Builder two;
  two.set_context(0xa010, 1).draw_auto(3).set_context(0xa010, 2).draw_auto(6);
  s = interpret(two.dwords().data(), two.dwords().size());
  check(s.draws.size() == 2 && s.draws[0].state.at(0xa010) == 1 && s.draws[1].state.at(0xa010) == 2, "interpret: two draws keep their own state");

  // Indexed draw.
  Builder ix;
  ix.index_type(0).draw_indexed(0x1'2345'6780ull, 100, 12).index_type(1).draw_indexed(0x1000, 100, 6);
  s = interpret(ix.dwords().data(), ix.dwords().size());
  check(s.draws.size() == 2 && s.draws[0].kind == DrawKind::Indexed && s.draws[0].index_size_bytes == 2 && s.draws[0].index_va == 0x123456780ull && s.draws[0].count == 12 && s.draws[1].index_size_bytes == 4, "interpret: DRAW_INDEX_2 with 16 and 32-bit index sizes and a 64-bit address");

  // Compute, writes, window violations, unknown opcodes.
  Builder m;
  m.dispatch(8, 4, 1).write_data(0xdead0000ull, 0xbeef).set_context(0xa010, 9).packet3(0x6f, {1, 2}).packet3(0x6f, {3});
  s = interpret(m.dwords().data(), m.dwords().size());
  check(s.dispatches.size() == 1 && s.dispatches[0].x == 8 && s.dispatches[0].y == 4 && s.writes.size() == 1 && s.writes[0].va == 0xdead0000ull && s.writes[0].data == 0xbeef, "interpret: dispatch and WRITE_DATA recorded");
  check(s.unhandled.at(0x6f) == 2 && report(s).find("UNHANDLED opcode 0x6f") != std::string::npos, "interpret: unknown opcodes are counted and reported, not guessed");
  { Builder w; w.set_reg(SET_CONTEXT_REG, 0xa3ff, {1, 2});  // runs off the end of the context window
    s = interpret(w.dwords().data(), w.dwords().size());
    check(s.regs.count(0xa3ff) == 1 && s.regs.count(0xa400) == 0 && !s.issues.empty(), "interpret: a write past the register window is stopped and reported"); }

  // CLEAR_STATE resets context registers only.
  Builder cs;
  cs.set_context(0xa010, 5).set_sh(0x2c10, 6).packet3(CLEAR_STATE, {0});
  s = interpret(cs.dwords().data(), cs.dwords().size());
  check(s.regs.count(0xa010) == 0 && s.regs.at(0x2c10) == 6, "interpret: CLEAR_STATE clears context registers and keeps SH registers");

  // Indirect buffers: resolver follows, no resolver is reported honestly, depth is bounded.
  Builder inner; inner.set_context(0xa010, 0x77).draw_auto(3);
  Builder outer; outer.indirect(0x5000, static_cast<std::uint32_t>(inner.dwords().size())).draw_auto(9);
  s = interpret(outer.dwords().data(), outer.dwords().size());
  check(s.indirect_unresolved == 1 && s.draws.size() == 1 && report(s).find("NOT followed") != std::string::npos, "indirect: without a resolver the buffer is counted and said to be not followed");
  s = interpret(outer.dwords().data(), outer.dwords().size(), [&](std::uint64_t va, std::size_t n) -> const std::uint32_t* { return va == 0x5000 && n == inner.dwords().size() ? inner.dwords().data() : nullptr; });
  check(s.draws.size() == 2 && s.draws[0].count == 3 && s.draws[0].state.at(0xa010) == 0x77 && s.draws[1].count == 9 && s.indirect_unresolved == 0, "indirect: a resolved buffer runs inline, in order");
  Builder loop; loop.indirect(0x9000, 4);
  s = interpret(loop.dwords().data(), loop.dwords().size(), [&](std::uint64_t, std::size_t) -> const std::uint32_t* { return loop.dwords().data(); });
  check(s.indirect_unresolved == 1, "indirect: self-referencing buffers stop at depth 4");

  check(std::string(opcode_name(0x2D)) == "DRAW_INDEX_AUTO" && std::string(opcode_name(0xEE)) == "?", "names: known and unknown opcodes");
  check(!describe(pk.empty() ? Packet{} : pk[0]).empty() && report(Summary{}).find("0 packets") != std::string::npos, "report: describe and report render");
}
