// Strict counted-loop tests. Synthetic RDNA words; real validation/readback.
// GPL-2.0. No commercial shader or hardware-GPU compatibility claim.
#include "shader/translate.hpp"
#include "graphics/spirv.hpp"
#include "graphics/vulkan.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>

void pnr_loop_tests(int& pass,int& fail) {
  auto check=[&](bool ok,const char* label){
    if(ok)++pass;else ++fail;std::printf("%s %s\n",ok?"PASS":"FAIL",label);
  };
  using namespace pnr::shader;using namespace pnr::graphics;
  constexpr uint64_t base=0x1200001000ull;
  auto add=[](uint32_t d,uint32_t a,uint32_t b){return (0x25u<<25u)|(d<<17u)|(b<<9u)|a;};
  auto mov=[](uint32_t d,uint32_t s){return 0x7e000200u|(d<<17u)|s;};
  auto kernel=[&](uint32_t lanes,uint32_t count,int shape) {
    std::vector<uint32_t> w;
    if(lanes==1)w={mov(0,128)}; // explicit lane zero in single mode
    w.insert(w.end(),{add(4,256,0),add(4,260,4),mov(2,255),0x1000,
      add(4,258,4),mov(5,255),0x12,0xdc300000,0x067f0004,
      mov(7,256),0xbe9403ff,count}); // v6=input, v7=lane, s20=count
    const uint32_t target=uint32_t(w.size()*4);
    // One recurrence: v6 += v7 (or s0), s20--, compare s20==0, backedge.
    w.insert(w.end(),{add(6,shape==1?0u:263u,6),0x80948114,0xbf068014,0});
    const uint32_t pc=uint32_t((w.size()-1)*4);
    w.back()=0xbf840000u|uint16_t(int32_t(target-pc-4)/4);
    if(shape==2)w.push_back(mov(6,20)); // verify exit counter state
    if(shape==3) { // guard after loop, pointer arithmetic result itself not affine
      w.insert(w.end(),{mov(8,260),mov(9,255),0x12,0xdc300000,0x067f0008});
    }
    w.insert(w.end(),{mov(2,255),lanes*4,add(4,258,4),
      0xdc700000,0x007f0604,0xbf810000});
    return w;
  };
  auto reject=[&](const std::vector<uint32_t>& w,const char* label,uint32_t lanes=8,uint32_t count=0){
    auto t=translate_compute_words(w,base,lanes*2+1,lanes,count);
    check(!t.ok && t.spirv.empty() && !t.error.empty(),label);
  };
  for(uint32_t n:{0u,65u,0xffffffffu})reject(kernel(8,n,0),"loop: zero/oversized/nonterminating count rejected");
  // Prefix has 12 words, then body (indices 12..15).
  auto w=kernel(8,8,0);w[10]=0xbe940300;
  reject(w,"loop: runtime count rejected rather than silently clamped",8,1);
  w=kernel(8,8,0);w[12]=add(6,262,6);
  reject(w,"loop: recurrence-dependent delta rejected");
  w=kernel(8,8,0);w[12]=add(6,20,6);
  reject(w,"loop: changing scalar counter delta rejected");
  w=kernel(8,8,0);w[12]=add(6,265,6);
  reject(w,"loop: undefined invariant delta rejected");
  w=kernel(8,8,0);w[12]=add(8,263,8);
  reject(w,"loop: undefined accumulator rejected");
  w=kernel(8,8,0);w[12]=add(6,263,7);
  reject(w,"loop: non-in-place accumulator body rejected");
  w=kernel(8,8,0);w[12]=0xdc300000;
  reject(w,"loop: body memory instruction rejected");
  w=kernel(8,8,0);w[12]=(0x26u<<25u)|(6u<<17u)|(6u<<9u)|263u;
  reject(w,"loop: body subtract outside narrow subset rejected");
  w=kernel(8,8,0);w[13]=0x80948214;
  reject(w,"loop: decrement by two rejected");
  w=kernel(8,8,0);w[13]=0x80958114;
  reject(w,"loop: different counter destination rejected");
  w=kernel(8,8,0);w[14]=0xbf068114;
  reject(w,"loop: comparison against nonzero rejected");
  w=kernel(8,8,0);w[14]=0xbf078014;
  reject(w,"loop: unsupported comparison rejected");
  w=kernel(8,8,0);w[15]=0xbf84fffb;
  reject(w,"loop: backedge to wrong instruction rejected");
  w=kernel(8,8,0);w[15]=0xbf85fffc;
  reject(w,"loop: unsupported branch condition rejected");
  w=kernel(8,8,0);{const std::vector<uint32_t> copy(w.begin()+10,w.begin()+16);w.insert(w.begin()+16,copy.begin(),copy.end());}
  reject(w,"loop: repeated loop rejected");
  w=kernel(8,8,0);w.insert(w.begin()+16,{0xbf060100,0xbf840001,mov(6,263)});
  reject(w,"loop: loop and conditional region combination rejected",8,2);
  w=kernel(8,8,0);w[12]=add(4,263,4);
  reject(w,"loop: loop-derived address loses affine proof");
  w=kernel(8,8,0);w[17]=0;
  reject(w,"loop: overlapping outputs and loads still rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopAdd,0,0,0}}).empty(),"loop: IR zero count rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopAdd,0,0,65}}).empty(),"loop: IR oversized count rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopAdd,2,0,2}}).empty(),"loop: IR forward accumulator ref rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopAdd,0,2,2}}).empty(),"loop: IR forward delta ref rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::Store,0,0},{U32Op::LoopAdd,0,1,2}}).empty(),"loop: IR store-value operand rejected");
  w=kernel(8,8,0);w[10]=0xbed403ff;
  reject(w,"loop: special scalar counter destination rejected");
  w=kernel(8,8,0);w[12]=add(6,249,6);
  reject(w,"loop: unsupported vector source modifier rejected");
  w=kernel(8,8,0);w[14]=0xbf068015;
  reject(w,"loop: compare uses wrong scalar counter rejected");
  w=kernel(8,8,0);w[13]=0x80948115;
  reject(w,"loop: decrement uses wrong scalar counter rejected");
  w=kernel(8,8,0);w.insert(w.begin()+16,{0x82158114});
  reject(w,"loop: post-loop addc cannot use stale arithmetic SCC");
  auto validate=[&](const ComputeTranslation& t) {
    const char* val=std::getenv("PNR_SPIRV_VAL");
    if(val && std::filesystem::exists(val)) {
      const auto path=(std::filesystem::temp_directory_path()/
        ("pnr_loop_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".spv")).string();
      {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
      check(t.ok && std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,
            "loop: loop header/continue/Phi module validates for Vulkan 1.0");
      std::filesystem::remove(path);
    }else std::puts("SKIP loop validator (not configured)");
  };
  VulkanBackend vk;auto st=vk.open();
  if(!st){std::printf("SKIP loop Vulkan: %s\n",st.error.message.c_str());return;}
  st=vk.create_instance("pnr-loop-tests");check(bool(st),"loop: instance created");
  if(!st || vk.devices().empty()){check(false,"loop: physical device available");return;}
  st=vk.create_device(0);check(bool(st),"loop: compute device created");if(!st)return;
  for(uint32_t lanes:{1u,2u,8u,64u})for(uint32_t count:{1u,2u,16u,64u}) {
    const auto code=kernel(lanes,count,0);
    auto t=translate_compute_words(code,base,lanes*2+1,lanes);
    check(t.ok && t.structured_loop && !t.stored_value_known,"loop: constant bounded loop compiles with runtime recurrence");
    bool loop_merge=false;for(size_t j=5;j<t.spirv.size();) {
      loop_merge |= (t.spirv[j]&65535u)==246;auto words=t.spirv[j]>>16u;if(!words)break;j+=words;
    }
    check(loop_merge,"loop: emitted actual OpLoopMerge, not unrolled/precomputed result");
    validate(t);
    for(uint32_t seed:{0u,0xffffffffu,0x80000000u}) {
      std::vector<uint32_t> io(lanes*2+1,0xa5a5a5a5);for(uint32_t j=0;j<lanes;++j)io[j]=seed+j;
      auto expected=io;for(uint32_t j=0;j<lanes;++j)expected[lanes+j]=uint32_t(io[j]+j*count);
      auto run=run_translated_compute(vk,t,&io);
      check(t.ok && bool(run) && io==expected,"loop: all trip counts/lanes wrap correctly; inputs and sentinel unchanged");
    }
    std::vector<uint8_t> bytes;for(auto word:code)for(unsigned j=0;j<4;++j)bytes.push_back(uint8_t(word>>(j*8)));
    auto bt=translate_compute_bytes(bytes,base,lanes*2+1,lanes);
    check(bt.ok && bt.spirv==t.spirv && bt.structured_loop,"loop: byte API preserves loop metadata and exact module");
  }
  // Inline count and inline/literal invariant delta forms, preserving word PCs.
  for(bool literal_delta:{false,true}) {
    auto code=kernel(8,2,0);
    code.erase(code.begin()+11);code[10]=0xbe940382; // s20=2 inline
    code[11]=add(6,literal_delta?255:129,6);
    if(literal_delta){code.insert(code.begin()+12,0xffffffff);code[15]=0xbf84fffb;}
    auto t=translate_compute_words(code,base,17,8);validate(t);
    std::vector<uint32_t> io(17,42),expected=io;
    for(uint32_t j=0;j<8;++j)expected[8+j]=literal_delta?40:44;
    auto run=run_translated_compute(vk,t,&io);
    check(t.ok && bool(run) && io==expected,"loop: inline count and literal/inline delta obey decoded PCs and wrap");
  }
  for(int shape:{1,2,3}) {
    auto t=translate_compute_words(kernel(8,64,shape),base,17,8,shape==1?1:0);
    validate(t);
    for(uint32_t delta:{0u,1u,0xffffffffu}) {
      std::vector<uint32_t> io(17,0xa5a5a5a5);for(uint32_t j=0;j<8;++j)io[j]=0xffffffffu-j;
      auto expected=io;for(uint32_t j=0;j<8;++j)expected[8+j]=shape==1?uint32_t(io[j]+delta*64):shape==2?0:io[j];
      auto run=run_translated_compute(vk,t,&io,shape==1?std::vector<uint32_t>{delta}:std::vector<uint32_t>{});
      check(t.ok && t.structured_loop && bool(run) && io==expected,"loop: uniform invariant delta, exit counter and post-loop guarded-load composition");
    }
  }
}
