// Compact deterministic shader audit fixtures. GPL-2.0.
// Prints actionable fixture/PC diagnostics; uses synthetic instruction words.
#include "shader/translate.hpp"
#include "shader/rdna.hpp"
#include "graphics/vulkan.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
struct Fixture {
  const char* name; std::vector<uint32_t> code; uint32_t words;
  std::vector<uint32_t> input, expected;
};
int main() {
  const std::vector<Fixture> cases={
    {"literal-store",{0x7e0002ff,0x1000,0x7e0202ff,0x12,0x7e0402ff,42,
      0xdc700000,0x007f0200,0xbf810000},1,{0},{42}},
    {"runtime-add",{0x7e0002ff,0x1000,0x7e0202ff,0x12,0xdc300000,0x027f0000,
      0x4a040481,0xdc700004,0x007f0200,0xbf810000},2,{41,0},{41,42}},
    {"guard-invalid-load",{0x7e0002ff,0x1000,0x7e0202ff,0x12,
      0xdc300000,0x047f0000,0x7e0a02ff,0x12,0xdc300000,0x027f0004,
      0xdc700004,0x007f0200,0xbf810000},2,{0xffffffff,99},{0xffffffff,0}}
  };
  int failed=0;
  pnr::graphics::VulkanBackend vk;
  auto st=vk.open();if(st)st=vk.create_instance("pnr-shader-fixtures");
  if(st && !vk.devices().empty())st=vk.create_device(0);
  const bool execute=bool(st) && !vk.devices().empty();
  if(!execute)std::printf("SKIP Vulkan execution: %s\n",st.error.message.c_str());
  for(const auto& f:cases) {
    auto t=pnr::shader::translate_compute_words(f.code,0x1200001000ull,f.words);
    if(!t.ok){std::printf("FAIL %s translate: %s\n",f.name,t.error.c_str());++failed;continue;}
    std::printf("PASS %s translated: input_words=%zu spirv_words=%zu guarded=%d\n",
                f.name,f.code.size(),t.spirv.size(),t.guarded_memory);
    if(execute) {
      auto io=f.input;auto run=pnr::shader::run_translated_compute(vk,t,&io);
      if(!run || io!=f.expected){++failed;std::printf("FAIL %s readback: %s\n",f.name,run.error.message.c_str());}
      else std::printf("PASS %s exact readback on %s\n",f.name,vk.device_name().c_str());
    }
  }
  // Deterministic decoder/translator mutation smoke test, not exhaustive fuzzing.
  uint32_t rng=0x504e52;size_t decoded=0,rejected=0;
  for(unsigned j=0;j<4096;++j) {
    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
    auto code=cases[j%cases.size()].code;
    code[j%code.size()]^=rng;
    auto d=pnr::shader::rdna_decode(code);decoded+=d.ok;
    auto t=pnr::shader::translate_compute_words(code,0x1200001000ull,2);
    if(!t.ok){++rejected;if(t.error.empty() || !t.spirv.empty())++failed;}
    else if(t.spirv.empty() || !t.required_buffer_words)++failed;
  }
  std::printf("mutation smoke: 4096 cases decoded=%zu rejected=%zu failures=%d\n",decoded,rejected,failed);
  return failed?1:0;
}
