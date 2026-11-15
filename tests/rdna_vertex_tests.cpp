// RDNA-translated vertex transform feeding the draw path. GPL-2.0.
#include "graphics/mat4.hpp"
#include "graphics/rdna_vertex.hpp"
#include "shader/rdna.hpp"
#include "graphics/demo/clip_shaders.hpp"
#include "graphics/demo/cube_shaders.hpp"
#include "graphics/demo/lit_shaders.hpp"
#include "graphics/demo/spin_shaders.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
void pnr_rdna_vertex_tests(int& pass,int& fail) {
  using namespace pnr::graphics;
  auto check=[&](bool ok,const char* label){if(ok)++pass;else ++fail;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
  std::vector<float> verts;std::vector<uint32_t> idx;cube_mesh(&verts,&idx);
  std::vector<float> xyz,col;
  for(size_t i=0;i<verts.size();i+=6){for(int k=0;k<3;++k){xyz.push_back(verts[i+k]);col.push_back(verts[i+3+k]);}}
  {
    auto w=rdna_vertex_component_kernel(0,24);
    auto t=pnr::shader::translate_compute_words(w,kRdnaVertexBase,96,24,16);
    check(t.ok&&t.host_float32&&t.required_uniform_words==16&&t.local_size_x==24,"rdna-vertex: kernel translates (float32, 16 uniform words, 24 lanes)");
    check(!pnr::shader::translate_compute_words(std::vector<uint32_t>(w.begin(),w.end()-2),kRdnaVertexBase,96,24,16).ok,"rdna-vertex: kernel without final store/end rejected");
  }
  // ---- slice S2: VOP3 v_fma_f32 (opcode 331, AMD public RDNA2 ISA) ----
  {
    auto w=rdna_vertex_component_kernel_fma(2,24);
    auto d=pnr::shader::rdna_decode(w);
    bool named=false;uint32_t fmas=0;
    for(const auto& in:d.instructions)
      if(in.family==pnr::shader::RdnaFamily::VOP3){
        ++fmas;
        named|=in.opcode_id==331&&in.mnemonic=="v_fma_f32"&&in.supported&&
               in.word_count==2&&in.dst.kind==pnr::shader::RdnaOperandKind::VectorReg&&
               in.dst.reg==14&&in.src2.kind==pnr::shader::RdnaOperandKind::VectorReg&&in.src2.reg==14;
      }
    check(d.ok&&fmas==3&&named,"rdna-fma: decoder names v_fma_f32 with dst/src2 operands");
    auto d64=pnr::shader::rdna_decode({0xd4000000u|(332u<<16u),0u});
    check(d64.ok&&d64.instructions.size()==1&&d64.instructions[0].mnemonic.empty()&&
          !d64.instructions[0].supported,"rdna-fma: neighbouring VOP3 opcode stays unnamed/unsupported");
    auto t=pnr::shader::translate_compute_words(w,kRdnaVertexBase,96,24,16);
    check(t.ok&&t.host_float32&&t.required_uniform_words==16&&t.local_size_x==24,"rdna-fma: fma kernel translates (float32, 16 uniform words, 24 lanes)");
    if(const char* val=std::getenv("PNR_SPIRV_VAL")){
      const std::string tmp="/tmp/pnr_fma_"+std::to_string(::getpid())+".spv";
      {std::ofstream o(tmp,std::ios::binary);o.write(reinterpret_cast<const char*>(t.spirv.data()),t.spirv.size()*4);}
      const std::string cmd=std::string(val)+" "+tmp+" 2>&1";
      FILE* p=popen(cmd.c_str(),"r");std::string out;char buf[256];
      while(p&&fgets(buf,sizeof buf,p))out+=buf;
      const int rc=p?pclose(p):-1;
      check(rc==0,"rdna-fma: spirv-val accepts the fused module");
      if(rc!=0)std::printf("    spirv-val said: %s\n",out.c_str());
      std::remove(tmp.c_str());
    }
    auto bad=w;bool any=false;
    for(size_t k=0;k+1<bad.size();++k)
      if((bad[k]&0xfc000000u)==0xd4000000u){any=true;bad[k]|=1u<<8u;break;}  // ABS src0
    check(any&&!pnr::shader::translate_compute_words(bad,kRdnaVertexBase,96,24,16).ok,"rdna-fma: ABS modifier rejected");
    bad=w;for(size_t k=0;k+1<bad.size();++k)
      if((bad[k]&0xfc000000u)==0xd4000000u){bad[k]|=1u<<15u;break;}  // CLMP
    check(!pnr::shader::translate_compute_words(bad,kRdnaVertexBase,96,24,16).ok,"rdna-fma: CLMP modifier rejected");
    bad=w;for(size_t k=0;k+1<bad.size();++k)
      if((bad[k]&0xfc000000u)==0xd4000000u){bad[k+1]|=1u<<29u;break;}  // NEG src0
    check(!pnr::shader::translate_compute_words(bad,kRdnaVertexBase,96,24,16).ok,"rdna-fma: NEG modifier rejected");
    bad=w;for(size_t k=0;k+1<bad.size();++k)
      if((bad[k]&0xfc000000u)==0xd4000000u){bad[k+1]|=1u<<27u;break;}  // OMOD
    check(!pnr::shader::translate_compute_words(bad,kRdnaVertexBase,96,24,16).ok,"rdna-fma: OMOD modifier rejected");
  }
  VulkanBackend vk;auto st=vk.open();
  if(!st){std::printf("SKIP rdna-vertex render: %s\n",st.error.message.c_str());return;}
  st=vk.create_instance("pnr-rdna-vertex-tests");
  if(!st||vk.devices().empty()){check(false,"rdna-vertex: device available");return;}
  st=vk.create_device(0);check(bool(st),"rdna-vertex: device created");if(!st)return;
  std::vector<uint32_t> hvs(std::begin(cube_vert),std::end(cube_vert)),cvs(std::begin(clip_vert),std::end(clip_vert)),fs(std::begin(spin_frag),std::end(spin_frag));
  const float black[4]={0,0,0,1};
  constexpr uint32_t W=96,H=96,N=8;
  const Mat4 P=mat4_perspective(1.0471976f,1.0f,0.1f,10.0f),T=mat4_translate(0,0,-3);
  // identity matrix returns the points with w=1 exactly
  {
    std::vector<float> clip;auto s=rdna_transform_points(vk,xyz,mat4_words(mat4_identity()),&clip);
    bool ok=bool(s)&&clip.size()==96;
    for(size_t i=0;ok&&i<24;++i)ok=clip[i*4]==xyz[i*3]&&clip[i*4+1]==xyz[i*3+1]&&clip[i*4+2]==xyz[i*3+2]&&clip[i*4+3]==1.0f;
    check(ok,"rdna-vertex: identity matrix reproduces positions exactly with w=1");
  }
  // translated output vs CPU float reference for every frame
  std::vector<std::vector<uint8_t>> ref,got;
  float worst=0;bool numeric=true;
  for(uint32_t k=0;k<N;++k){
    Mat4 M=mat4_mul(mat4_rotate_x(0.5f),mat4_rotate_y(k*2.0f*(float)M_PI/N));
    Mat4 mvp=mat4_mul(P,mat4_mul(T,M));
    std::vector<float> clip;auto s=rdna_transform_points(vk,xyz,mat4_words(mvp),&clip);
    if(!s){std::printf("  rdna-vertex error: %s\n",s.error.message.c_str());numeric=false;break;}
    for(size_t i=0;i<24;++i){
      auto r=mat4_apply(mvp,xyz[i*3],xyz[i*3+1],xyz[i*3+2]);
      for(int c=0;c<4;++c)worst=std::max(worst,std::fabs(r[c]-clip[i*4+c]));
    }
    std::vector<float> cv;for(size_t i=0;i<24;++i){for(int c=0;c<4;++c)cv.push_back(clip[i*4+c]);for(int c=0;c<3;++c)cv.push_back(col[i*3+c]);}
    std::vector<uint8_t> img;
    auto r=vk.render_clip_mesh_rgba(W,H,cvs,fs,cv,idx,true,black,&img);
    if(!r){std::printf("  rdna-vertex draw error: %s\n",r.error.message.c_str());numeric=false;break;}
    got.push_back(img);
  }
  check(numeric&&got.size()==N,"rdna-vertex: 8 frames transformed by translated RDNA and drawn via clip-space path");
  std::printf("  rdna-vertex: max |clip - CPU float| over 8 frames x 24 verts x 4 comps = %.3g\n",(double)worst);
  check(numeric&&worst<1e-5f,"rdna-vertex: translated clip positions match CPU float reference (<1e-5)");
  std::vector<std::vector<uint32_t>> push;
  for(uint32_t k=0;k<N;++k){Mat4 M=mat4_mul(mat4_rotate_x(0.5f),mat4_rotate_y(k*2.0f*(float)M_PI/N));push.push_back(mat4_words(mat4_mul(P,mat4_mul(T,M))));}
  auto rs=vk.render_mesh_frames_rgba(W,H,hvs,fs,verts,idx,true,push,black,&ref);
  check(bool(rs)&&ref.size()==N&&got.size()==N,"rdna-vertex: hand-authored vertex-shader reference frames rendered");
  if(rs&&got.size()==N){
    uint32_t total=0,worstf=0;bool same_cov=true;
    for(uint32_t k=0;k<N;++k){
      uint32_t d=0;
      for(size_t p=0;p<size_t(W)*H;++p){bool df=false;for(int c=0;c<3;++c)df|=ref[k][p*4+c]!=got[k][p*4+c];d+=df;}
      std::printf("  rdna-vertex frame %u: pixels differing from hand-authored VS = %u of %u\n",k,d,W*H);
      total+=d;worstf=std::max(worstf,d);
    }
    check(worstf<=W*H/200,"rdna-vertex: every frame within 0.5% of pixels vs hand-authored VS (measured 0 differing on lavapipe)");
    std::printf("  rdna-vertex: total differing pixels %u of %u\n",total,N*W*H);
    (void)same_cov;
    if(const char* pp=std::getenv("PNR_RDNA_PPM")){
      std::ofstream o(pp,std::ios::binary);o<<"P6\n"<<W*N<<" "<<H<<"\n255\n";
      for(uint32_t y=0;y<H;++y)for(uint32_t k=0;k<N;++k)for(uint32_t x=0;x<W;++x)o.write(reinterpret_cast<const char*>(&got[k][(y*W+x)*4]),3);
    }
  }
  // ---- slice S2: fused multiply-add vertex path executed on the GPU ----
  {
    auto fma_ref=[&](const Mat4&m,const float*p,uint32_t c){
      float r=std::fma(p[2],m[8+c],m[12+c]);
      r=std::fma(p[1],m[4+c],r);
      r=std::fma(p[0],m[c],r);
      return r;
    };
    float fworst=0,cross=0;bool fnum=true;
    std::vector<std::vector<uint8_t>> fgot;
    for(uint32_t k=0;k<N;++k){
      Mat4 M=mat4_mul(mat4_rotate_x(0.5f),mat4_rotate_y(k*2.0f*(float)M_PI/N));
      Mat4 mvp=mat4_mul(P,mat4_mul(T,M));
      std::vector<float> clip,fclip;
      auto s=rdna_transform_points(vk,xyz,mat4_words(mvp),&fclip,true);
      auto s2=rdna_transform_points(vk,xyz,mat4_words(mvp),&clip);
      if(!s||!s2){std::printf("  rdna-fma error: %s\n",(s?s2:s).error.message.c_str());fnum=false;break;}
      for(size_t i=0;i<24;++i)for(uint32_t c=0;c<4;++c){
        fworst=std::max(fworst,std::fabs(fma_ref(mvp,&xyz[i*3],c)-fclip[i*4+c]));
        cross=std::max(cross,std::fabs(clip[i*4+c]-fclip[i*4+c]));
      }
      std::vector<float> cv;for(size_t i=0;i<24;++i){for(int c=0;c<4;++c)cv.push_back(fclip[i*4+c]);for(int c=0;c<3;++c)cv.push_back(col[i*3+c]);}
      std::vector<uint8_t> img;
      auto r=vk.render_clip_mesh_rgba(W,H,cvs,fs,cv,idx,true,black,&img);
      if(!r){std::printf("  rdna-fma draw error: %s\n",r.error.message.c_str());fnum=false;break;}
      fgot.push_back(img);
    }
    check(fnum&&fgot.size()==N,"rdna-fma: 8 frames transformed by translated v_fma_f32 and drawn via clip-space path");
    std::printf("  rdna-fma: max |fma clip - CPU std::fma| over 8 frames x 24 verts x 4 comps = %.3g\n",(double)fworst);
    check(fnum&&fworst<1e-6f,"rdna-fma: fused clip positions match CPU std::fma reference (<1e-6)");
    std::printf("  rdna-fma: max |fma clip - mul/add clip| = %.3g\n",(double)cross);
    check(fnum&&cross<1e-5f,"rdna-fma: fused and mul/add paths agree (<1e-5)");
    if(rs&&ref.size()==N&&fgot.size()==N){
      uint32_t worstf=0;
      for(uint32_t k=0;k<N;++k){
        uint32_t d=0;
        for(size_t p=0;p<size_t(W)*H;++p){bool df=false;for(int c=0;c<3;++c)df|=ref[k][p*4+c]!=fgot[k][p*4+c];d+=df;}
        worstf=std::max(worstf,d);
      }
      std::printf("  rdna-fma: worst frame differs from hand-authored VS by %u of %u pixels\n",worstf,W*H);
      check(worstf<=W*H/200,"rdna-fma: every fused frame within 0.5% of pixels vs hand-authored VS");
      if(const char* pp=std::getenv("PNR_RDNA_FMA_PPM")){
        std::ofstream o(pp,std::ios::binary);o<<"P6\n"<<W*N<<" "<<H<<"\n255\n";
        for(uint32_t y=0;y<H;++y)for(uint32_t k=0;k<N;++k)for(uint32_t x=0;x<W;++x)o.write(reinterpret_cast<const char*>(&fgot[k][(y*W+x)*4]),3);
      }
    }
  }
  // ---- per-vertex Lambert dot via the translated dot3 kernel ----
  {
    auto tk=pnr::shader::translate_compute_words(rdna_dot3_kernel(24),kRdnaVertexBase,96,24,3);
    check(tk.ok&&tk.host_float32&&tk.required_uniform_words==3,"rdna-light: dot3 kernel translates (float32, 3 uniform words)");
    std::vector<float> nrm;for(size_t i=0;i<24;++i){double a=0.37*i,b=0.21*i+0.5;nrm.push_back((float)(std::cos(a)*std::cos(b)));nrm.push_back((float)std::sin(b));nrm.push_back((float)(std::sin(a)*std::cos(b)));}
    const float L[3]={0.3f,-0.5f,0.8124f};std::vector<float> dd;
    auto s=rdna_light_dots(vk,nrm,L,&dd);
    float worst=0;bool ok=bool(s)&&dd.size()==24;
    for(size_t i=0;ok&&i<24;++i){float r=(L[0]*nrm[i*3]+L[1]*nrm[i*3+1])+L[2]*nrm[i*3+2];worst=std::max(worst,std::fabs(r-dd[i]));}
    std::printf("  rdna-light: max |kernel - CPU float dot| over 24 normals = %.3g\n",(double)worst);
    check(ok&&worst<1e-6f,"rdna-light: translated dot3 matches the CPU float dot (<1e-6)");
    check(!rdna_light_dots(vk,{},L,&dd).ok,"rdna-light: empty normals rejected");
    check(!rdna_light_dots(vk,std::vector<float>(75,0.0f),L,&dd).ok,"rdna-light: more than 24 normals rejected");
    check(!rdna_light_dots(vk,nrm,L,nullptr).ok,"rdna-light: null output rejected");
    std::vector<uint8_t> img;std::vector<std::vector<uint8_t>> frs;
    std::vector<uint32_t> lvs(std::begin(lit_clip_vert),std::end(lit_clip_vert)),lh(std::begin(lit_hand_vert),std::end(lit_hand_vert)),lf(std::begin(lit_frag),std::end(lit_frag));
    check(!vk.render_lit_clip_mesh_rgba(W,H,lvs,lf,std::vector<float>(9),idx,true,black,&img).ok,"rdna-light: lit clip vertex data not multiple of 8 floats rejected");
    check(!vk.render_lit_clip_mesh_rgba(W,H,lvs,lf,std::vector<float>(8),idx,true,black,nullptr).ok,"rdna-light: lit clip null image rejected");
    check(!vk.render_lit_mesh_frames_rgba(W,H,lh,lf,std::vector<float>(10),idx,true,{std::vector<uint32_t>(20)},black,&frs).ok,"rdna-light: lit mesh vertex data not multiple of 9 floats rejected");
    check(!vk.render_lit_mesh_frames_rgba(W,H,lh,lf,std::vector<float>(9*24),idx,true,{std::vector<uint32_t>(33)},black,&frs).ok,"rdna-light: lit push constants over 32 words rejected");
  }
  std::vector<float> clip;
  check(!rdna_transform_points(vk,{},mat4_words(mat4_identity()),&clip).ok,"rdna-vertex: empty point list rejected");
  check(!rdna_transform_points(vk,xyz,std::vector<uint32_t>(15),&clip).ok,"rdna-vertex: wrong matrix size rejected");
  std::vector<float> many(3*25,0.0f);
  check(!rdna_transform_points(vk,many,mat4_words(mat4_identity()),&clip).ok,"rdna-vertex: more than 24 points rejected");
  check(!vk.render_clip_mesh_rgba(W,H,cvs,fs,std::vector<float>(6),idx,true,black,nullptr).ok,"rdna-vertex: null image rejected");
  std::vector<uint8_t> junk;
  check(!vk.render_clip_mesh_rgba(W,H,cvs,fs,std::vector<float>(13),idx,true,black,&junk).ok,"rdna-vertex: clip vertex data not multiple of 7 floats rejected");
}
