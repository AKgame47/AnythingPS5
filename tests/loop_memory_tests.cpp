// Two-axis memory-proof tests, GPL-2.0. No loop-memory lowering implied.
#include "shader/loop_memory.hpp"
#include <cstdio>
#include <limits>
#include <map>
#include <random>
void pnr_loop_memory_tests(int& pass,int& fail) {
  using namespace pnr::shader;
  auto check=[&](bool ok,const char* label){if(ok)++pass;else ++fail;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
  auto bad=[&](const std::vector<LoopMemoryAccess>& a,uint32_t w,uint32_t l,uint32_t i,const char* label){
    auto r=prove_loop_memory(a,w,l,i);check(!r.ok && !r.error.empty() && !r.checked_points,label);
  };
  for(uint32_t n:{0u,65537u})bad({{true,0,4,256}},n,1,1,"loop-proof: invalid SSBO extent rejected");
  for(uint32_t n:{0u,65u}) {
    bad({{true,0,4,256}},65536,n,1,"loop-proof: invalid lane extent rejected");
    bad({{true,0,4,256}},65536,1,n,"loop-proof: invalid iteration extent rejected");
  }
  bad({},65536,1,1,"loop-proof: empty access list rejected");
  bad(std::vector<LoopMemoryAccess>(33),65536,1,1,"loop-proof: access budget enforced");
  for(auto a:{LoopMemoryAccess{true,1,4,32},LoopMemoryAccess{true,0,1,32},LoopMemoryAccess{true,0,4,33}})
    bad({a},65536,8,8,"loop-proof: origin/lane/iteration misalignment rejected");
  const auto max=std::numeric_limits<uint64_t>::max();
  for(auto a:{LoopMemoryAccess{true,0,max-3,4},LoopMemoryAccess{true,0,4,max-3},
             LoopMemoryAccess{true,max-3,4,0},LoopMemoryAccess{true,0,max-3,4}})
    bad({a},65536,4,4,"loop-proof: multiplication/addition overflow rejected without wrap");
  bad({{true,0,4,32}},15,8,2,"loop-proof: last iteration out of range rejected");
  bad({{true,0,0,0}},64,2,2,"loop-proof: broadcast store collision rejected");
  bad({{true,0,4,4}},64,2,2,"loop-proof: cross-iteration diagonal store collision rejected");
  bad({{false,4,4,32},{true,0,4,32}},64,8,2,"loop-proof: cross-lane read/write alias rejected");
  bad({{false,32,4,32},{true,0,4,32}},64,8,2,"loop-proof: cross-iteration read/write alias rejected");
  bad({{false,0,4,32},{true,0,4,32}},64,8,2,"loop-proof: same-lane same-iteration read/write alias rejected");
  bad({{true,0,4,32},{true,32,4,32}},64,8,2,"loop-proof: inter-access output collision rejected");
  auto r=prove_loop_memory({{false,0,0,0},{false,0,0,0}},1,64,64);
  check(r.ok && r.checked_points==8192,"loop-proof: repeated read/read alias permitted across both axes");
  r=prove_loop_memory({{true,0,1,3}},1,1,1);
  check(r.ok && r.checked_points==1,"loop-proof: inactive stride axes need not be aligned");
  for(uint32_t lanes:{1u,2u,8u,64u})for(uint32_t count:{1u,2u,16u,64u}) {
    const uint64_t block=uint64_t(lanes)*count*4;
    r=prove_loop_memory({{false,0,4,uint64_t(lanes)*4},{true,block,4,uint64_t(lanes)*4}},uint32_t(block/2),lanes,count);
    check(r.ok && r.checked_points==lanes*count*2,
          "loop-proof: contiguous 2-axis input/output tiles proven disjoint at exact extent");
    bad({{false,0,4,uint64_t(lanes)*4},{true,block,4,uint64_t(lanes)*4}},uint32_t(block/2)-1,lanes,count,
        "loop-proof: one-word-short tiled buffer rejected");
    r=prove_loop_memory({{true,0,uint64_t(count)*4,4}},uint32_t(block/4),lanes,count);
    check(r.ok,"loop-proof: transposed axis map proven without hardcoded stride-four lane assumption");
  }
  // Thirty-two maximum-size maps: 131072 exact points; no timings claimed.
  std::vector<LoopMemoryAccess> largest;
  for(uint32_t k=0;k<32;++k)largest.push_back({true,uint64_t(k)*4096,4,256});
  bad(largest,65536,64,64,"loop-proof: maximum map diagonal inter-access collisions rejected");
  largest.clear();for(uint32_t k=0;k<16;++k)largest.push_back({true,uint64_t(k)*16384,4,256});
  r=prove_loop_memory(largest,65536,64,64);
  check(r.ok && r.checked_points==65536,"loop-proof: largest SSBO fully covered by distinct maps");
  // Independent small-domain oracle uses a map of word addresses and pair
  // events. It does not call or share the production sorter/max-bound proof.
  std::mt19937 rng(0x504e5232);
  bool all=true;uint32_t tested=0,accepted=0,rejected=0;
  for(unsigned trial=0;trial<12000;++trial) {
    const uint32_t lanes=1+rng()%4,iters=1+rng()%4,words=1+rng()%32,n=1+rng()%4;
    std::vector<LoopMemoryAccess> a;std::map<uint64_t,std::pair<unsigned,unsigned>> seen;
    bool expected=true;
    for(uint32_t k=0;k<n;++k) {
      LoopMemoryAccess x{bool(rng()%2),rng()%80,rng()%20,rng()%20};
      if(trial%2){x.origin&=~uint64_t(3);x.lane_stride&=~uint64_t(3);x.iteration_stride&=~uint64_t(3);}
      a.push_back(x);
      for(uint32_t l=0;l<lanes;++l)for(uint32_t i=0;i<iters;++i) {
        const auto offset=x.origin+l*x.lane_stride+i*x.iteration_stride;
        if(offset%4 || offset>=uint64_t(words)*4)expected=false;
        auto& counts=seen[offset];if(x.store)++counts.second;else ++counts.first;
      }
    }
    for(const auto& [offset,counts]:seen) {
      (void)offset;if(counts.second && counts.first+counts.second>1)expected=false;
    }
    if(expected)++accepted;else ++rejected;
    const auto proof=prove_loop_memory(a,words,lanes,iters);
    all &= proof.ok==expected && (proof.ok?proof.error.empty() && proof.checked_points==n*lanes*iters:!proof.error.empty() && !proof.checked_points);
    ++tested;
  }
  check(all && tested==12000 && accepted>100 && rejected>100,"loop-proof: 12000 deterministic affine maps match independent address-event oracle");
}
