// Offscreen render pixel tests (software Vulkan), GPL-2.0.
#include "graphics/vulkan.hpp"
#include "graphics/demo/triangle_shaders.hpp"
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
  // Optional visual artifact for humans: PNR_RENDER_PPM=/path/out.ppm
  if(const char* p=std::getenv("PNR_RENDER_PPM")){
    std::vector<uint8_t> big;const float c3[4]={0.05f,0.08f,0.25f,1};
    if(vk.render_offscreen_rgba(256,256,vs,fs,3,c3,&big)){
      std::ofstream f(p,std::ios::binary);f<<"P6\n256 256\n255\n";
      for(size_t i=0;i<big.size();i+=4)f.write(reinterpret_cast<char*>(&big[i]),3);
    }
  }
}
