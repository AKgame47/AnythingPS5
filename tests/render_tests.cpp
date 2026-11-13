// Offscreen render pixel tests (software Vulkan), GPL-2.0.
#include "graphics/vulkan.hpp"
#include "graphics/demo/triangle_shaders.hpp"
#include "graphics/demo/spin_shaders.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
void pnr_render_tests(int& pass,int& fail) {
  using namespace pnr::graphics;
  auto check=[&](bool ok,const char* label){if(ok)++pass;else ++fail;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
  VulkanBackend vk;auto st=vk.open();
  if(!st){std::printf("SKIP offscreen render: %s\n",st.error.message.c_str());return;}
  st=vk.create_instance("pnr-render-tests");
  if(!st||vk.devices().empty()){check(false,"render: device available");return;}
  st=vk.create_device(0);check(bool(st),"render: device created");if(!st)return;
  std::vector<uint32_t> vs(std::begin(triangle_vert),std::end(triangle_vert)),
                        fs(std::begin(triangle_frag),std::end(triangle_frag));
  const float clear[4]={0.0f,0.0f,1.0f,1.0f};
  constexpr uint32_t W=64,H=64;
  std::vector<uint8_t> img;
  st=vk.render_offscreen_rgba(W,H,vs,fs,3,clear,&img);
  if(!st)std::printf("  render error: %s\n",st.error.message.c_str());
  check(bool(st)&&img.size()==W*H*4,"render: 64x64 RGBA8 triangle rendered and read back");
  if(!st||img.size()!=W*H*4)return;
  auto px=[&](uint32_t x,uint32_t y,uint8_t* o){for(int i=0;i<4;++i)o[i]=img[(y*W+x)*4+i];};
  auto is=[&](uint32_t x,uint32_t y,uint8_t r,uint8_t g,uint8_t b,uint8_t a){uint8_t p[4];px(x,y,p);return p[0]==r&&p[1]==g&&p[2]==b&&p[3]==a;};
  check(is(32,32,255,0,0,255),"render: center pixel is the red fragment colour");
  check(is(32,16,255,0,0,255),"render: pixel inside the upper edge is red");
  bool corners=is(0,0,0,0,255,255)&&is(W-1,0,0,0,255,255)&&is(0,H-1,0,0,255,255)&&is(W-1,H-1,0,0,255,255);
  check(corners,"render: all four corners keep the blue clear colour");
  check(is(4,60,0,0,255,255)&&is(60,60,0,0,255,255),"render: points outside the triangle are clear colour");
  uint32_t red=0,blue=0,other=0;
  for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
    if(is(x,y,255,0,0,255))++red;else if(is(x,y,0,0,255,255))++blue;else ++other;}
  // Analytic area: base 1.5 x height 1.5 / 2 = 1.125 of the 4.0 NDC square
  // = 28.125% of 4096 = 1152 pixels. Allow +-8% for edge rasterisation rules.
  check(other==0,"render: every pixel is exactly red or the clear colour");
  check(red>=1060&&red<=1245,"render: red coverage matches the analytic triangle area");
  std::printf("  coverage red=%u clear=%u (expected ~1152)\n",red,blue);
  // Shape: the triangle narrows toward the bottom (vertex at y=+0.75 is bottom row in Vulkan).
  auto rowred=[&](uint32_t y){uint32_t n=0;for(uint32_t x=0;x<W;++x)if(is(x,y,255,0,0,255))++n;return n;};
  check(rowred(14)>rowred(32)&&rowred(32)>rowred(50)&&rowred(50)>0,"render: row widths shrink top to bottom (correct orientation)");
  // Determinism and a different size / clear colour.
  std::vector<uint8_t> img2;const float c2[4]={0,1,0,1};
  st=vk.render_offscreen_rgba(W,H,vs,fs,3,clear,&img2);
  check(bool(st)&&img2==img,"render: second render is byte-identical");
  st=vk.render_offscreen_rgba(100,50,vs,fs,3,c2,&img2);
  check(bool(st)&&img2.size()==100*50*4&&img2[0]==0&&img2[1]==255&&img2[2]==0,"render: non-square size with green clear");
  // Honest rejection paths.
  std::vector<uint8_t> junk;
  check(!vk.render_offscreen_rgba(0,8,vs,fs,3,clear,&junk).ok,"render: zero size rejected");
  check(!vk.render_offscreen_rgba(8,8,vs,fs,4,clear,&junk).ok,"render: vertex count not multiple of 3 rejected");
  check(!vk.render_offscreen_rgba(8,8,{},fs,3,clear,&junk).ok,"render: empty shader rejected");
  std::vector<uint32_t> bad(vs.begin(),vs.begin()+6);
  check(!vk.render_offscreen_rgba(8,8,bad,fs,3,clear,&junk).ok,"render: truncated SPIR-V rejected by driver, no crash");
  // ---- animated slice: vertex buffer + per-frame push constants ----
  {
    std::vector<uint32_t> svs(std::begin(spin_vert),std::end(spin_vert)),sfs(std::begin(spin_frag),std::end(spin_frag));
    const std::vector<float> tri={-0.6f,-0.6f,1,0,0,  0.6f,-0.6f,0,1,0,  0.0f,0.6f,0,0,1};
    const float black[4]={0,0,0,1};
    constexpr uint32_t N=8;
    std::vector<std::vector<uint32_t>> push;
    auto bits=[](float f){uint32_t u;std::memcpy(&u,&f,4);return u;};
    for(uint32_t k=0;k<N;++k){double a=k*2.0*M_PI/N;push.push_back({bits((float)std::cos(a)),bits((float)std::sin(a))});}
    std::vector<std::vector<uint8_t>> fr;
    auto s2=vk.render_frames_rgba(W,H,svs,sfs,tri,push,black,&fr);
    if(!s2)std::printf("  spin error: %s\n",s2.error.message.c_str());
    bool shape=bool(s2)&&fr.size()==N;for(auto&f:fr)shape=shape&&f.size()==W*H*4;
    check(shape,"spin: 8 frames of 64x64 RGBA8 rendered with vertex buffer + push constants");
    if(shape){
      auto at=[&](uint32_t f,uint32_t x,uint32_t y,int c){return (int)fr[f][(y*W+x)*4+c];};
      bool cov_ok=true,cen_ok=true,alpha_ok=true;
      for(uint32_t k=0;k<N;++k){
        double sx=0,sy=0;uint32_t n=0;
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
          if(at(k,x,y,3)!=255)alpha_ok=false;
          if(at(k,x,y,0)+at(k,x,y,1)+at(k,x,y,2)>0){sx+=x+0.5;sy+=y+0.5;++n;}}
        double a=k*2.0*M_PI/N;
        // centroid (0,-0.2) rotated by a, mapped to pixels
        double ex=(0.2*std::sin(a)+1.0)*32.0,ey=(-0.2*std::cos(a)+1.0)*32.0;
        if(n<693||n>781)cov_ok=false;  // analytic 737.28 +-6%
        if(n==0||std::fabs(sx/n-ex)>1.0||std::fabs(sy/n-ey)>1.0)cen_ok=false;
        std::printf("  spin frame %u: pixels=%u centroid=(%.2f,%.2f) expected=(%.2f,%.2f)\n",k,n,n?sx/n:0.0,n?sy/n:0.0,ex,ey);
      }
      check(alpha_ok,"spin: every pixel of every frame has alpha 255");
      check(cov_ok,"spin: coverage stays within 6% of the analytic area in all 8 rotations");
      check(cen_ok,"spin: covered-pixel centroid follows the analytic rotation within 1 px");
      check(at(0,32,25,0)>60&&at(0,32,25,0)<110&&at(0,32,25,1)>60&&at(0,32,25,1)<110&&at(0,32,25,2)>60&&at(0,32,25,2)<110,"spin: frame 0 centroid pixel is the interpolated grey-ish mix of R,G,B");
      check(at(0,14,14,0)>150&&at(0,14,14,1)<80&&at(0,14,14,2)<80,"spin: frame 0 red vertex corner is red");
      check(at(2,49,14,0)>150&&at(2,49,14,1)<80&&at(2,49,14,2)<80,"spin: frame 2 (90 deg) red vertex moved to top-right");
      check(at(4,49,49,0)>150&&at(4,49,49,1)<80&&at(4,49,49,2)<80,"spin: frame 4 (180 deg) red vertex moved to bottom-right");
      uint32_t diff=0;for(size_t i=0;i<fr[0].size();i+=4)if(fr[0][i]!=fr[1][i]||fr[0][i+1]!=fr[1][i+1]||fr[0][i+2]!=fr[1][i+2])++diff;
      check(diff>150,"spin: consecutive frames differ (animation changes pixels)");
      std::vector<std::vector<uint8_t>> again;
      check(vk.render_frames_rgba(W,H,svs,sfs,tri,push,black,&again)&&again==fr,"spin: re-rendering the sequence is byte-identical");
      if(const char* p=std::getenv("PNR_SPIN_PPM")){
        std::vector<std::vector<uint8_t>> big;std::vector<std::vector<uint32_t>> bp(push);
        if(vk.render_frames_rgba(128,128,svs,sfs,tri,bp,black,&big)){
          std::ofstream f(p,std::ios::binary);f<<"P6\n"<<128*N<<" 128\n255\n";
          for(uint32_t y=0;y<128;++y)for(uint32_t k=0;k<N;++k)for(uint32_t x=0;x<128;++x)f.write(reinterpret_cast<char*>(&big[k][(y*128+x)*4]),3);
        }
      }
    }
    std::vector<std::vector<uint8_t>> junk2;
    auto bad_v=tri;bad_v.push_back(0);
    check(!vk.render_frames_rgba(W,H,svs,sfs,bad_v,push,black,&junk2).ok,"spin: vertex data not a multiple of 5 floats rejected");
    auto bad_p=push;bad_p[3].push_back(0);
    check(!vk.render_frames_rgba(W,H,svs,sfs,tri,bad_p,black,&junk2).ok,"spin: inconsistent push-constant sizes rejected");
    check(!vk.render_frames_rgba(W,H,svs,sfs,tri,{},black,&junk2).ok,"spin: zero frames rejected");
    check(!vk.render_frames_rgba(W,H,svs,sfs,tri,std::vector<std::vector<uint32_t>>(65,push[0]),black,&junk2).ok,"spin: 65 frames rejected");
  }
  // Optional visual artifact for humans: PNR_RENDER_PPM=/path/out.ppm
  if(const char* p=std::getenv("PNR_RENDER_PPM")){
    std::vector<uint8_t> big;const float c3[4]={0.05f,0.08f,0.25f,1};
    if(vk.render_offscreen_rgba(256,256,vs,fs,3,c3,&big)){
      std::ofstream f(p,std::ios::binary);f<<"P6\n256 256\n255\n";
      for(size_t i=0;i<big.size();i+=4)f.write(reinterpret_cast<char*>(&big[i]),3);
    }
  }
}
