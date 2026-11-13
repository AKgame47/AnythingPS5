#include "shader/loop_memory.hpp"
#include <algorithm>
#include <limits>
namespace pnr::shader {
LoopMemoryProof prove_loop_memory(const std::vector<LoopMemoryAccess>& accesses,
                                 uint32_t words,uint32_t lanes,uint32_t iterations) {
  auto fail=[](const char* error){LoopMemoryProof r;r.error=error;return r;};
  if(!words || words>65536)return fail("loop SSBO words must be 1..65536");
  if(!lanes || lanes>64 || !iterations || iterations>64)
    return fail("loop lane/iteration extent must be 1..64");
  if(accesses.empty() || accesses.size()>32)return fail("loop access count must be 1..32");
  struct Point {uint64_t offset;bool store;};
  std::vector<Point> points;points.reserve(accesses.size()*lanes*iterations);
  constexpr auto max=std::numeric_limits<uint64_t>::max();
  const uint64_t last=uint64_t(words-1)*4;
  for(const auto& a:accesses) {
    // Check products and addition before evaluating: no modulo proof values.
    // Stride alignment matters only on an axis which actually advances.
    if(a.origin%4 || (lanes>1 && a.lane_stride%4) ||
       (iterations>1 && a.iteration_stride%4))return fail("unaligned loop address map");
    const uint64_t lane=lanes-1,iter=iterations-1;
    if((lane && a.lane_stride>max/lane) ||
       (iter && a.iteration_stride>max/iter))return fail("loop stride product overflow");
    const uint64_t ld=lane*a.lane_stride,id=iter*a.iteration_stride;
    if(a.origin>max-ld || a.origin+ld>max-id)return fail("loop address sum overflow");
    if(a.origin+ld+id>last)return fail("loop address map outside SSBO extent");
    // Max proof above bounds every point for nonnegative strides. Exact
    // enumeration detects diagonal aliasing (lane and iteration can cancel
    // in pairwise differences even when both strides are nonzero).
    for(uint32_t l=0;l<lanes;++l)for(uint32_t i=0;i<iterations;++i)
      points.push_back({a.origin+uint64_t(l)*a.lane_stride+uint64_t(i)*a.iteration_stride,a.store});
  }
  std::sort(points.begin(),points.end(),[](const Point& a,const Point& b){return a.offset<b.offset;});
  for(size_t start=0;start<points.size();) {
    size_t end=start+1;bool store=points[start].store;
    while(end<points.size() && points[end].offset==points[start].offset){store |= points[end].store;++end;}
    if(store && end-start>1)return fail("loop store collision or read/write alias across lane/iteration/access");
    start=end;
  }
  LoopMemoryProof r;r.ok=true;r.checked_points=uint32_t(points.size());return r;
}
} // namespace pnr::shader
