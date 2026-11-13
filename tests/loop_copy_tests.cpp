// Proof-checked guarded copy loop execution tests, GPL-2.0.
#include "shader/translate.hpp"
#include "graphics/spirv.hpp"
#include "graphics/vulkan.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
void pnr_loop_copy_tests(int& pass,int& fail) {
  using namespace pnr::shader;using namespace pnr::graphics;
  auto check=[&](bool ok,const char* label){if(ok)++pass;else ++fail;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
  constexpr uint64_t base=0x1200001000ull;
  auto add=[](uint32_t d,uint32_t a,uint32_t b){return (0x25u<<25u)|(d<<17u)|(b<<9u)|a;};
  auto mov=[](uint32_t d,uint32_t s){return 0x7e000200u|(d<<17u)|s;};
  auto kernel=[&](uint32_t lanes,uint32_t count,bool runtime,bool offset) {
    const uint32_t domain=runtime?64:count,block=lanes*domain*4;
    std::vector<uint32_t> w;
    if(lanes==1)w={mov(0,128)};
    w.insert(w.end(),{add(4,256,0),add(4,260,4),mov(2,255),0x1000,add(4,258,4),
      mov(5,255),0x12,mov(2,255),block,add(8,258,4),mov(9,255),0x12});
    // src v4:v5, dest v8:v9, data v6, counter s20.
    if(runtime)w.push_back(0xbe940300);else w.insert(w.end(),{0xbe9403ff,count});
    const uint32_t target=uint32_t(w.size()*4);
    w.insert(w.end(),{0xdc300000u+(offset?4u:0u),0x067f0004,
      0xdc700000u+(offset?4u:0u),0x007f0608,
      add(4,255,4),lanes*4,add(8,255,8),lanes*4,
      0x80948114,0xbf068014,0});
    const uint32_t pc=uint32_t((w.size()-1)*4);
    w.back()=0xbf840000u|uint16_t(int32_t(target-pc-4)/4);w.push_back(0xbf810000);
    return w;
  };
  auto reject=[&](const std::vector<uint32_t>& w,uint32_t words,uint32_t lanes,const char* label,uint32_t uniforms=0,uint64_t b=0x1200001000ull){
    auto r=translate_compute_words(w,b,words,lanes,uniforms);check(!r.ok && r.spirv.empty() && !r.error.empty(),label);
  };
  auto w=kernel(8,2,false,false);
  // 0..11 prefix, 12..13 init, 14..24 body, 25 end.
  reject(w,31,8,"loop-copy: final iteration store OOB rejected");
  auto bad=w;bad[8]=0;
  reject(bad,32,8,"loop-copy: input/output exact alias rejected");
  bad=w;bad[8]=4;
  reject(bad,32,8,"loop-copy: cross-lane/iteration alias rejected");
  bad=w;bad[19]=4;bad[21]=4;
  reject(bad,32,8,"loop-copy: diagonal store collisions rejected");
  bad=w;bad[19]=1;
  reject(bad,32,8,"loop-copy: misaligned iteration stride rejected");
  bad=w;bad[19]=0xfffffffc;
  reject(bad,32,8,"loop-copy: pointer recurrence overflow rejected");
  bad=w;bad[14]|=0x800;
  reject(bad,32,8,"loop-copy: reserved FLAT offset bit rejected");
  bad=w;bad[15]|=0x100;
  reject(bad,32,8,"loop-copy: unsupported load VDATA field rejected");
  bad=w;bad[17]=0x007f0708;
  reject(bad,32,8,"loop-copy: store data not loaded value rejected");
  bad=w;bad[15]=0x047f0004;bad[17]=0x007f0408;
  reject(bad,32,8,"loop-copy: load overwrites address low rejected");
  bad=w;bad[15]=0x057f0004;bad[17]=0x007f0508;
  reject(bad,32,8,"loop-copy: load overwrites address high rejected");
  bad=w;bad[17]=0x007f0605;
  reject(bad,32,8,"loop-copy: overlapping pointer pairs rejected");
  bad=w;bad[18]=add(7,255,4);
  reject(bad,32,8,"loop-copy: increment wrong destination rejected");
  bad=w;bad[18]=add(4,255,8);
  reject(bad,32,8,"loop-copy: increment wrong pointer source rejected");
  bad=w;bad[18]=add(4,262,4);bad.erase(bad.begin()+19);
  reject(bad,32,8,"loop-copy: runtime pointer step rejected");
  bad=w;bad[22]=0x80948214;
  reject(bad,32,8,"loop-copy: wrong decrement rejected");
  bad=w;bad[23]=0xbf068114;
  reject(bad,32,8,"loop-copy: wrong exit comparison rejected");
  bad=w;bad[24]=0xbf84fff8;
  reject(bad,32,8,"loop-copy: wrong backedge rejected");
  bad=w;bad[3]=0x1001;
  reject(bad,33,8,"loop-copy: unaligned affine origin rejected");
  bad=w;bad[6]=0x13;
  reject(bad,32,8,"loop-copy: constant high mismatch rejected");
  bad=w;bad[5]=mov(5,0);
  reject(bad,32,8,"loop-copy: dynamic high word rejected",1);
  bad=w;bad[4]=mov(4,0);
  reject(bad,32,8,"loop-copy: uniform low without affine proof rejected",1);
  bad=w;bad[13]=0;
  reject(bad,32,8,"loop-copy: zero do-while count rejected");
  bad=w;bad[13]=65;
  reject(bad,32,8,"loop-copy: excessive count rejected");
  auto runtime=kernel(8,2,true,false);
  reject(runtime,8*64*2-1,8,"loop-copy: runtime count proves full 64 iteration extent",1);
  reject(runtime,8*64*2,8,"loop-copy: undeclared count rejected");
  auto crossing=kernel(8,2,false,false);crossing[3]=0xfffffff0;
  reject(crossing,32,8,"loop-copy: low-word boundary-crossing binding rejected",0,0x12fffffff0ull);
  // A prior guest store cannot be combined with loop stores.
  bad=w;bad.insert(bad.begin()+12,{mov(6,129),0xdc700000,0x007f0608});
  reject(bad,32,8,"loop-copy: extra store outside copy loop rejected");
  bad=w;{const std::vector<uint32_t> body(w.begin()+12,w.begin()+25);bad.insert(bad.begin()+25,body.begin(),body.end());}
  reject(bad,32,8,"loop-copy: repeated memory loop rejected");
  bad=w;bad.insert(bad.begin()+25,{0xbf068014,0xbf840001,mov(6,128)});
  reject(bad,32,8,"loop-copy: combined conditional region rejected");
  bad=w;bad[17]=0x007f060a;
  reject(bad,32,8,"loop-copy: undefined destination pointer pair rejected");
  bad=w;bad[21]=0;
  reject(bad,32,8,"loop-copy: invariant output pointer collision rejected");
  // Low-level malformed IR must not emit pointer operations from bogus nodes.
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopy,0,0,0}}).empty(),"loop-copy: IR zero count rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopy,0,0,65}}).empty(),"loop-copy: IR excessive count rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopy,0,0,1,2}}).empty(),"loop-copy: IR mode rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopy,0,0,1,0,4,4,2048}}).empty(),"loop-copy: IR offset out of FLAT range rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopy,2,0,1}}).empty(),"loop-copy: IR forward source rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopyDest,0}}).empty(),"loop-copy: IR pointer projection from nonloop rejected");
  check(SpirvModule::compute_u32({{U32Op::Constant,0},{U32Op::LoopCopy,0,0,0,1}}).empty(),"loop-copy: IR runtime mode without guard rejected");
  auto validate=[&](const ComputeTranslation& t) {
    const char* val=std::getenv("PNR_SPIRV_VAL");
    if(val && std::filesystem::exists(val)) {
      const auto path=(std::filesystem::temp_directory_path()/("pnr_copy_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".spv")).string();
      {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
      check(t.ok && std::system((std::string(val)+" --target-env vulkan1.0 "+path).c_str())==0,"loop-copy: nested loop and load/store guards validate for Vulkan 1.0");
      std::filesystem::remove(path);
    }else std::puts("SKIP copy loop validator (not configured)");
  };
  VulkanBackend vk;auto st=vk.open();if(!st){std::printf("SKIP copy-loop Vulkan: %s\n",st.error.message.c_str());return;}
  st=vk.create_instance("pnr-copy-loop-tests");check(bool(st),"loop-copy: instance created");
  if(!st || vk.devices().empty()){check(false,"loop-copy: device available");return;}
  st=vk.create_device(0);check(bool(st),"loop-copy: compute device created");if(!st)return;
  for(uint32_t lanes:{1u,2u,8u,64u})for(uint32_t count:{1u,2u,16u,64u}) {
    auto code=kernel(lanes,count,false,false);const uint32_t block=lanes*count;
    auto t=translate_compute_words(code,base,block*2+1,lanes);validate(t);
    check(t.ok && t.loop_memory && t.structured_loop && t.guarded_memory,"loop-copy: guest memory shape passes two-axis proof and emits loop");
    for(uint32_t seed:{0u,0xffffffffu,0x12345678u}) {
      std::vector<uint32_t> io(block*2+1,0xa5a5a5a5);for(uint32_t j=0;j<block;++j)io[j]=seed+j;auto expected=io;
      for(uint32_t j=0;j<block;++j)expected[block+j]=io[j];
      auto run=run_translated_compute(vk,t,&io);
      check(t.ok && bool(run) && io==expected,"loop-copy: every iteration and lane copies correctly; inputs/sentinel unchanged");
    }
    std::vector<uint8_t> bytes;for(auto word:code)for(unsigned j=0;j<4;++j)bytes.push_back(uint8_t(word>>(j*8)));
    auto bt=translate_compute_bytes(bytes,base,block*2+1,lanes);check(bt.ok && bt.loop_memory && bt.spirv==t.spirv,"loop-copy: byte API preserves loop-memory metadata");
  }
  for(uint32_t lanes:{1u,8u,64u}) {
    const uint32_t block=lanes*64;
    auto t=translate_compute_words(kernel(lanes,1,true,false),base,block*2+1,lanes,1);validate(t);
    for(uint32_t count:{1u,2u,16u,64u}) {
      std::vector<uint32_t> io(block*2+1,0xa5a5a5a5);for(uint32_t j=0;j<block;++j)io[j]=0xffffffffu-j;auto expected=io;
      for(uint32_t j=0;j<lanes*count;++j)expected[block+j]=io[j];
      auto run=run_translated_compute(vk,t,&io,{count});
      check(t.ok && t.loop_memory && bool(run) && io==expected,"loop-copy: runtime count copies exact prefix, later output untouched");
    }
    for(uint32_t count:{0u,65u,0xffffffffu}) {
      std::vector<uint32_t> io(block*2+1,7),before=io;
      auto run=run_translated_compute(vk,t,&io,{count});
      check(!run && run.error.code==105 && io==before,"loop-copy: invalid count rejected before memory dispatch");
      auto raw=vk.run_compute_words(t.spirv,&io,{count});check(t.ok && bool(raw) && io==before,"loop-copy: raw invalid count guard precedes loop memory effects");
    }
  }
  // Offsets are included in both proof maps, not mistaken for pointer steps.
  {
    const uint32_t lanes=8,count=4,block=lanes*count;
    auto t=translate_compute_words(kernel(lanes,count,false,true),base,block*2+2,lanes);validate(t);
    std::vector<uint32_t> io(block*2+2,0xcafebabe);for(uint32_t j=1;j<=block;++j)io[j]=j*17;
    auto expected=io;for(uint32_t j=1;j<=block;++j)expected[block+j]=io[j];
    auto run=run_translated_compute(vk,t,&io);
    check(t.ok && bool(run) && io==expected,"loop-copy: nonzero FLAT offsets included in proof and per-iteration guard");
  }
  // Low-level modules make final-state projections and defensive guards
  // observable without widening the guest one-store-instruction subset.
  for(int shape=0;shape<5;++shape) {
    std::vector<U32Node> nodes={{U32Op::Constant,shape==3?0xffcu:0x1000u},
      {U32Op::Constant,shape==4?0x1020u:0x1010u},
      {U32Op::LoopCopy,0,1,2,0,4,4}};
    const uint32_t data=2;
    if(shape<2)nodes.push_back({shape==0?U32Op::LoopCopySource:U32Op::LoopCopyDest,2});
    nodes.push_back({U32Op::Store,shape<2?3u:data,7});
    auto module=SpirvModule::compute_u32(nodes,8,0x1000);
    ComputeTranslation t;t.ok=!module.empty();t.spirv=module;validate(t);
    std::vector<uint32_t> io={10,20,30,40,0xaaaa,0xbbbb,0xcccc,0xdddd};auto expected=io;
    if(shape==3){expected[4]=0;expected[5]=10;expected[7]=10;}
    else if(shape==4)expected[7]=20;
    else {expected[4]=10;expected[5]=20;expected[7]=shape==0?0x1008:shape==1?0x1018:20;}
    auto raw=vk.run_compute_words(module,&io);
    check(bool(raw) && io==expected,"loop-copy: final pointers/data and invalid load=zero/store=skip are observable");
  }

  {
    auto code=kernel(8,4,false,false);code[19]=4;code[21]=4;
    code.insert(code.begin()+2,{add(4,260,4),add(4,260,4)});
    auto t=translate_compute_words(code,base,65,8);validate(t);
    std::vector<uint32_t> io(65,0xa5a5a5a5);for(uint32_t j=0;j<32;++j)io[j]=j*31;
    auto expected=io;for(uint32_t j=0;j<32;++j)expected[32+j]=io[j];auto run=run_translated_compute(vk,t,&io);
    check(t.ok && bool(run) && io==expected,"loop-copy: transposed lane/iteration map executes under exact proof");
  }

}
