// 3D cube slice tests: mat4 math, depth buffer, MVP via push constants,
// indexed mesh. Pixels are checked against an independent CPU ray caster.
#include "graphics/mat4.hpp"
#include "graphics/vulkan.hpp"
#include "graphics/demo/cube_shaders.hpp"
#include "graphics/demo/spin_shaders.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>
#include <unistd.h>
#include "loader/import_resolver.hpp"
#include "loader/native_loader.hpp"
#include "prx/prx.hpp"
void pnr_cube_tests(int& pass,int& fail) {
  using namespace pnr::graphics;
  auto check=[&](bool ok,const char* label){if(ok)++pass;else ++fail;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
  auto near=[](float a,float b,float e=1e-4f){return std::fabs(a-b)<=e;};
  // ---- mat4 math (no GPU needed) ----
  {
    Mat4 id=mat4_identity();Mat4 t=mat4_translate(1,2,3);
    check(mat4_mul(id,t)==t&&mat4_mul(t,id)==t,"mat4: identity is neutral for multiplication");
    auto p=mat4_apply(mat4_rotate_y(1.5707963f),1,0,0);
    check(near(p[0],0)&&near(p[2],-1),"mat4: rotate_y(90 deg) maps +x to -z");
    auto q=mat4_apply(mat4_rotate_x(1.5707963f),0,1,0);
    check(near(q[1],0)&&near(q[2],1),"mat4: rotate_x(90 deg) maps +y to +z");
    Mat4 P=mat4_perspective(1.0471976f,1.0f,0.1f,10.0f);
    auto n=mat4_apply(P,0,0,-0.1f),f=mat4_apply(P,0,0,-10.0f);
    check(near(n[2]/n[3],0.0f)&&near(f[2]/f[3],1.0f,1e-3f),"mat4: perspective maps near plane to depth 0 and far plane to depth 1");
    auto up=mat4_apply(P,0,1,-2);
    check(up[1]/up[3]<0,"mat4: world +y lands at negative Vulkan NDC y (up on screen)");
    std::vector<float> v;std::vector<uint32_t> ix;cube_mesh(&v,&ix);
    bool ok=v.size()==24*6&&ix.size()==36;for(auto i:ix)ok=ok&&i<24;
    for(size_t i=0;i<v.size();i+=6)ok=ok&&(std::fabs(std::fabs(v[i])-0.5f)<1e-6f||std::fabs(std::fabs(v[i+1])-0.5f)<1e-6f||std::fabs(std::fabs(v[i+2])-0.5f)<1e-6f);
    check(ok,"cube mesh: 24 vertices on the faces of a unit cube, 36 valid indices");
  }
  VulkanBackend vk;auto st=vk.open();
  if(!st){std::printf("SKIP cube render: %s\n",st.error.message.c_str());return;}
  st=vk.create_instance("pnr-cube-tests");
  if(!st||vk.devices().empty()){check(false,"cube: device available");return;}
  st=vk.create_device(0);check(bool(st),"cube: device created");if(!st)return;
  std::vector<uint32_t> vs(std::begin(cube_vert),std::end(cube_vert)),fs(std::begin(spin_frag),std::end(spin_frag));
  std::vector<float> verts;std::vector<uint32_t> idx;cube_mesh(&verts,&idx);
  const float black[4]={0,0,0,1};
  constexpr uint32_t W=96,H=96;
  const float fov=1.0471976f;
  const Mat4 P=mat4_perspective(fov,1.0f,0.1f,10.0f),T=mat4_translate(0,0,-3);
  // ---- front view, depth on vs off ----
  {
    auto push=std::vector<std::vector<uint32_t>>{mat4_words(mat4_mul(P,T))};
    std::vector<std::vector<uint8_t>> on,off;
    auto s1=vk.render_mesh_frames_rgba(W,H,vs,fs,verts,idx,true,push,black,&on);
    auto s2=vk.render_mesh_frames_rgba(W,H,vs,fs,verts,idx,false,push,black,&off);
    if(!s1)std::printf("  cube error: %s\n",s1.error.message.c_str());
    check(bool(s1)&&bool(s2)&&on.size()==1&&off.size()==1&&on[0].size()==W*H*4,"cube: front view rendered with depth on and off");
    if(s1&&s2){
      auto px=[&](const std::vector<uint8_t>& im,uint32_t x,uint32_t y,int c){return (int)im[(y*W+x)*4+c];};
      auto is=[&](const std::vector<uint8_t>& im,uint32_t x,uint32_t y,int r,int g,int b){return px(im,x,y,0)==r&&px(im,x,y,1)==g&&px(im,x,y,2)==b;};
      check(is(on[0],48,48,255,0,0),"cube: depth ON centre pixel is the FRONT (+Z, red) face");
      check(is(off[0],48,48,255,255,0),"cube: depth OFF centre pixel is the BACK (-Z, yellow) face drawn later (painter order)");
      uint32_t yel=0;for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x)if(is(on[0],x,y,255,255,0))++yel;
      check(yel==0,"cube: depth ON hides the back face completely");
      check(on[0]!=off[0],"cube: depth buffer changes the image");
      // analytic bbox of the front face (z=+0.5): project its corners
      Mat4 mvp=mat4_mul(P,T);double lo=1e9,hi=-1e9;
      for(float sx:{-0.5f,0.5f}){auto c=mat4_apply(mvp,sx,0.5f,0.5f);double px_=(c[0]/c[3]+1)*0.5*W;lo=std::min(lo,px_);hi=std::max(hi,px_);}
      uint32_t mn=W,mx=0;for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x)if(is(on[0],x,y,255,0,0)){mn=std::min(mn,x);mx=std::max(mx,x);}
      std::printf("  front face x-extent: rendered %u..%u, analytic %.2f..%.2f\n",mn,mx,lo,hi);
      check(std::fabs(mn-lo)<=1.0&&std::fabs((mx+1.0)-hi)<=1.0,"cube: front face extent matches the analytic projection within 1 px");
    }
  }
  // ---- rotating sequence vs CPU ray caster ----
  {
    constexpr uint32_t N=8;
    std::vector<Mat4> models;std::vector<std::vector<uint32_t>> push;
    for(uint32_t k=0;k<N;++k){Mat4 m=mat4_mul(mat4_rotate_x(0.5f),mat4_rotate_y(k*2.0f*(float)M_PI/N));models.push_back(m);push.push_back(mat4_words(mat4_mul(P,mat4_mul(T,m))));}
    std::vector<std::vector<uint8_t>> fr;
    auto s=vk.render_mesh_frames_rgba(W,H,vs,fs,verts,idx,true,push,black,&fr);
    if(!s)std::printf("  cube error: %s\n",s.error.message.c_str());
    bool shape=bool(s)&&fr.size()==N;check(shape,"cube: 8-frame rotating sequence rendered (MVP push constants, depth on)");
    if(shape){
      // colour table by (axis, sign): +x green, -x magenta, +y blue, -y cyan, +z red, -z yellow
      const int col[3][2][3]={{{255,0,255},{0,255,0}},{{0,255,255},{0,0,255}},{{255,255,0},{255,0,0}}};
      const double f=1.0/std::tan(fov*0.5);
      bool all_ok=true,faces_seen[3][2]={};uint32_t worst=0;
      for(uint32_t k=0;k<N;++k){
        const Mat4& M=models[k];
        double R[3][3];for(int r=0;r<3;++r)for(int c=0;c<3;++c)R[r][c]=M[c*4+r];
        uint32_t mism=0,hits=0,cov=0;
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
          double nx=(x+0.5)/W*2-1,ny=(y+0.5)/H*2-1;
          double dv[3]={nx/f,-ny/f,-1.0},ov[3]={0,0,3};
          double o[3],d[3];
          for(int i=0;i<3;++i){o[i]=R[0][i]*ov[0]+R[1][i]*ov[1]+R[2][i]*ov[2];d[i]=R[0][i]*dv[0]+R[1][i]*dv[1]+R[2][i]*dv[2];}  // R^T * v
          double tn=-1e30,tf=1e30;int ax=-1,sg=0;
          for(int i=0;i<3;++i){
            double t1=(-0.5-o[i])/d[i],t2=(0.5-o[i])/d[i];
            double lo=std::min(t1,t2),hi=std::max(t1,t2);
            if(lo>tn){tn=lo;ax=i;sg=d[i]>0?0:1;}
            tf=std::min(tf,hi);
          }
          bool hit=tn<tf&&tf>0;
          const uint8_t* p=&fr[k][(y*W+x)*4];
          bool lit=(p[0]|p[1]|p[2])!=0;if(lit)++cov;
          if(hit){++hits;faces_seen[ax][sg]=true;}
          bool same=hit?(p[0]==col[ax][sg][0]&&p[1]==col[ax][sg][1]&&p[2]==col[ax][sg][2]):!lit;
          if(!same)++mism;
        }
        worst=std::max(worst,mism);
        std::printf("  cube frame %u: ray-cast hits=%u rendered=%u mismatched=%u of %u\n",k,hits,cov,mism,W*H);
        if(mism>W*H/100)all_ok=false;
      }
      check(all_ok,"cube: every frame matches the CPU ray caster (per-pixel face colour and occlusion) within 1% of pixels");
      int nf=0;for(auto&a:faces_seen)for(bool b:a)nf+=b;
      check(nf>=5,"cube: rotation exposes at least 5 of the 6 faces over the sequence");
      std::vector<std::vector<uint8_t>> again;
      check(vk.render_mesh_frames_rgba(W,H,vs,fs,verts,idx,true,push,black,&again)&&again==fr,"cube: re-rendering the sequence is byte-identical");
      if(const char* pp=std::getenv("PNR_CUBE_PPM")){
        std::vector<std::vector<uint8_t>> big;
        if(vk.render_mesh_frames_rgba(128,128,vs,fs,verts,idx,true,push,black,&big)){
          std::ofstream o(pp,std::ios::binary);o<<"P6\n"<<128*N<<" 128\n255\n";
          for(uint32_t y=0;y<128;++y)for(uint32_t k=0;k<N;++k)for(uint32_t x=0;x<128;++x)o.write(reinterpret_cast<char*>(&big[k][(y*128+x)*4]),3);
        }
      }
    }
  }
  // ---- the real program path: a game process drives the rotation ----
#ifdef PNR_PS_CUBE_DEMO_PATH
  {
    const std::string jail="/tmp/pnr_cubedemo_jail_"+std::to_string(::getpid());
    std::filesystem::remove_all(jail);
    pnr::loader::NativeLoader loader;
    auto lr=loader.launch(PNR_PS_CUBE_DEMO_PATH,{},{"PNR_JAIL_ROOT="+jail,std::string("LD_LIBRARY_PATH=")+PNR_PSABI_DIR});
    check(lr.validated&&lr.started&&lr.finished&&!lr.signaled&&lr.exit_code==0,"cube game: ps_cube_demo launched as a game process, exit 0");
    // frames the game presented must equal the harness render of the same MVPs
    constexpr uint32_t N=8;bool all_same=true;uint32_t found=0;
    std::vector<std::vector<uint32_t>> push;
    for(uint32_t k=0;k<N;++k){Mat4 m=mat4_mul(mat4_rotate_x(0.5f),mat4_rotate_y(k*2.0f*(float)M_PI/N));push.push_back(mat4_words(mat4_mul(P,mat4_mul(T,m))));}
    std::vector<std::vector<uint8_t>> ref;
    bool rendered=bool(vk.render_mesh_frames_rgba(W,H,vs,fs,verts,idx,true,push,black,&ref))&&ref.size()==N;
    for(uint32_t k=0;k<N&&rendered;++k){
      char name[64];std::snprintf(name,sizeof name,"/frames/cube_%02u.ppm",k);
      std::ifstream f(jail+name,std::ios::binary);
      if(!f){all_same=false;continue;}
      ++found;
      std::stringstream ss;ss<<f.rdbuf();std::string d=ss.str();
      const std::string hdr="P6\n96 96\n255\n";
      if(d.size()!=hdr.size()+W*H*3||d.compare(0,hdr.size(),hdr)!=0){all_same=false;continue;}
      for(uint32_t i=0;i<W*H;++i)for(int c=0;c<3;++c)
        if((uint8_t)d[hdr.size()+i*3+c]!=ref[k][i*4+c])all_same=false;
    }
    check(rendered&&found==N,"cube game: all 8 presented PPM files exist inside the game's jail");
    check(rendered&&all_same,"cube game: presented frames (RDNA-translated vertex path) are byte-identical to the harness hand-authored-VS render of the same MVPs");
    pnr::prx::PrxRegistry reg;pnr::loader::ImportResolution ir;
    check(pnr::loader::resolve_imports(PNR_PS_CUBE_DEMO_PATH,reg,&ir)&&ir.pnr_extension==7&&ir.unknown==0,"cube game: pnrGfx* imports classify as pnr-extension (not host, not sce)");
    std::filesystem::remove_all(jail);
  }
#endif
#ifdef PNR_PS_MESH_DEMO_PATH
  // ---- a game uploads its own mesh (pnrGfxSetMesh) ----
  {
    const std::string jail="/tmp/pnr_meshdemo_jail_"+std::to_string(::getpid());
    std::filesystem::remove_all(jail);
    pnr::loader::NativeLoader loader;
    auto lr=loader.launch(PNR_PS_MESH_DEMO_PATH,{},{"PNR_JAIL_ROOT="+jail,std::string("LD_LIBRARY_PATH=")+PNR_PSABI_DIR});
    check(lr.validated&&lr.started&&lr.finished&&!lr.signaled&&lr.exit_code==0,"mesh game: ps_mesh_demo launched, exit 0 (9 invalid uploads refused, RDNA vs reference compared inside the game)");
    const float py[5][3]={{0,0.6f,0},{-0.5f,-0.4f,-0.5f},{0.5f,-0.4f,-0.5f},{0.5f,-0.4f,0.5f},{-0.5f,-0.4f,0.5f}};
    const int tri[6][3]={{0,1,2},{0,2,3},{0,3,4},{0,4,1},{1,3,2},{1,4,3}};
    constexpr uint32_t N=8;bool all_ok=true;uint32_t found=0,worst=0;
    for(uint32_t k=0;k<N;++k){
      char name[64];std::snprintf(name,sizeof name,"/frames/mesh_%02u.ppm",k);
      std::ifstream f(jail+name,std::ios::binary);
      if(!f){all_ok=false;continue;}
      ++found;std::stringstream ss;ss<<f.rdbuf();std::string d=ss.str();
      const std::string hdr="P6\n96 96\n255\n";
      if(d.size()!=hdr.size()+W*H*3){all_ok=false;continue;}
      Mat4 M=mat4_mul(mat4_rotate_x(0.4f),mat4_rotate_y(k*2.0f*(float)M_PI/N));
      Mat4 mvp=mat4_mul(P,mat4_mul(T,M));
      std::array<float,4> c[5];for(int i=0;i<5;++i)c[i]=mat4_apply(mvp,py[i][0],py[i][1],py[i][2]);
      uint32_t mism=0,cov=0;
      for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        const double px_=(x+0.5)/W*2-1,py_=(y+0.5)/H*2-1;bool in=false;
        for(auto& tr:tri){
          double X[3],Y[3];for(int j=0;j<3;++j){X[j]=c[tr[j]][0]/c[tr[j]][3];Y[j]=c[tr[j]][1]/c[tr[j]][3];}
          double e0=(X[1]-X[0])*(py_-Y[0])-(Y[1]-Y[0])*(px_-X[0]);
          double e1=(X[2]-X[1])*(py_-Y[1])-(Y[2]-Y[1])*(px_-X[1]);
          double e2=(X[0]-X[2])*(py_-Y[2])-(Y[0]-Y[2])*(px_-X[2]);
          if((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0)){in=true;break;}
        }
        const unsigned char* q=reinterpret_cast<const unsigned char*>(&d[hdr.size()+(y*W+x)*3]);
        const bool lit=(q[0]|q[1]|q[2])!=0;if(lit)++cov;
        if(lit!=in)++mism;
      }
      worst=std::max(worst,mism);
      std::printf("  mesh game frame %u: CPU-projected mask vs presented pixels: %u mismatched, %u lit\n",k,mism,cov);
      if(mism>W*H/100)all_ok=false;
    }
    check(found==N,"mesh game: all 8 presented PPM files exist inside the game's jail");
    check(all_ok,"mesh game: presented silhouette matches an independent CPU projection of the uploaded triangles within 1% of pixels");
    pnr::prx::PrxRegistry reg;pnr::loader::ImportResolution ir;
    check(pnr::loader::resolve_imports(PNR_PS_MESH_DEMO_PATH,reg,&ir)&&ir.pnr_extension==6&&ir.unknown==0,"mesh game: pnrGfx* imports classify as pnr-extension");
    std::filesystem::remove_all(jail);
  }
#endif
#ifdef PNR_PS_TEX_DEMO_PATH
  // ---- a game uploads a texture + uv quad; CPU ray-cast + sampler reference ----
  {
    const std::string jail="/tmp/pnr_texdemo_jail_"+std::to_string(::getpid());
    std::filesystem::remove_all(jail);
    pnr::loader::NativeLoader loader;
    auto lr=loader.launch(PNR_PS_TEX_DEMO_PATH,{},{"PNR_JAIL_ROOT="+jail,std::string("LD_LIBRARY_PATH=")+PNR_PSABI_DIR});
    check(lr.validated&&lr.started&&lr.finished&&!lr.signaled&&lr.exit_code==0,"texture game: ps_tex_demo launched, exit 0 (9 invalid calls refused inside the game)");
    // 8x8 texture the game builds: r=32x+16, g=32y+16, b=(x+y odd?255:64)
    auto texel=[&](int x,int y,int c)->double{return c==0?32.0*x+16:c==1?32.0*y+16:(((x+y)&1)?255.0:64.0);};
    const double f=1.0/std::tan(fov*0.5);
    constexpr uint32_t N=8;bool all_ok=true;uint32_t found=0;
    double worst_frac=0;uint32_t distinct_nearest=0;
    for(uint32_t k=0;k<N;++k){
      char name[64];std::snprintf(name,sizeof name,"/frames/tex_%02u.ppm",k);
      std::ifstream fi(jail+name,std::ios::binary);
      if(!fi){all_ok=false;continue;}
      ++found;std::stringstream ss;ss<<fi.rdbuf();std::string d=ss.str();
      const std::string hdr="P6\n96 96\n255\n";
      if(d.size()!=hdr.size()+W*H*3){all_ok=false;continue;}
      const bool lin=k>=4;const double us=lin?2.0:1.0;
      Mat4 M=mat4_mul(mat4_rotate_x(0.3f),mat4_rotate_y((k%4)*0.35f-0.5f));
      double R[3][3];for(int r=0;r<3;++r)for(int c=0;c<3;++c)R[r][c]=M[c*4+r];
      uint32_t mism=0,cov=0;std::set<int> seen;
      for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        double nx=(x+0.5)/W*2-1,ny=(y+0.5)/H*2-1;
        double dv[3]={nx/f,-ny/f,-1.0},ov[3]={0,0,3};double o[3],dd[3];
        for(int i=0;i<3;++i){o[i]=R[0][i]*ov[0]+R[1][i]*ov[1]+R[2][i]*ov[2];dd[i]=R[0][i]*dv[0]+R[1][i]*dv[1]+R[2][i]*dv[2];}
        const unsigned char* q=reinterpret_cast<const unsigned char*>(&d[hdr.size()+(y*W+x)*3]);
        const double t=-o[2]/dd[2];
        const double px=o[0]+t*dd[0],py=o[1]+t*dd[1];
        const bool hit=t>0&&std::fabs(px)<=0.7&&std::fabs(py)<=0.7;
        if(!hit){if(q[0]|q[1]|q[2])++mism;continue;}
        ++cov;
        const double u=(px+0.7)/1.4*us,v=(0.7-py)/1.4*us;
        double ref[3];
        if(!lin){
          int tx=std::min(7,std::max(0,(int)std::floor(u*8))),ty=std::min(7,std::max(0,(int)std::floor(v*8)));
          for(int c=0;c<3;++c)ref[c]=texel(tx,ty,c);
          seen.insert(ty*8+tx);
        }else{
          const double fu=u*8-0.5,fv=v*8-0.5;
          const int x0=(int)std::floor(fu),y0=(int)std::floor(fv);
          const double ax=fu-x0,ay=fv-y0;
          auto wrap=[](int i){return ((i%8)+8)%8;};
          for(int c=0;c<3;++c){
            double a=texel(wrap(x0),wrap(y0),c),b=texel(wrap(x0+1),wrap(y0),c),cc=texel(wrap(x0),wrap(y0+1),c),dq=texel(wrap(x0+1),wrap(y0+1),c);
            ref[c]=(a*(1-ax)+b*ax)*(1-ay)+(cc*(1-ax)+dq*ax)*ay;
          }
        }
        bool same=true;for(int c=0;c<3;++c)if(std::fabs(q[c]-ref[c])>(lin?4.5:0.5))same=false;
        if(!same)++mism;
      }
      if(k==0||k==4)distinct_nearest=std::max<uint32_t>(distinct_nearest,(uint32_t)seen.size());
      const double frac=(double)mism/(double)W/(double)H;worst_frac=std::max(worst_frac,frac);
      std::printf("  texture game frame %u (%s): CPU sampler reference vs presented pixels: %u mismatched of %u, %u quad pixels\n",k,lin?"linear/repeat":"nearest/clamp",mism,W*H,cov);
      if(mism>W*H*3/100||cov<1000)all_ok=false;
    }
    check(found==N,"texture game: all 8 presented PPM files exist inside the game's jail");
    check(all_ok,"texture game: presented pixels match an independent CPU ray-cast + nearest/bilinear sampler within tolerance (nearest +-0, linear +-4, <=3% of pixels)");
    check(distinct_nearest>=20,"texture game: the nearest frame shows many distinct texels (texture really sampled, not a flat colour)");
    pnr::prx::PrxRegistry reg;pnr::loader::ImportResolution ir;
    check(pnr::loader::resolve_imports(PNR_PS_TEX_DEMO_PATH,reg,&ir)&&ir.pnr_extension==7&&ir.unknown==0,"texture game: pnrGfx* imports classify as pnr-extension");
    std::filesystem::remove_all(jail);
  }
#endif
#ifdef PNR_PS_LIT_DEMO_PATH
  // ---- a game uploads a lit mesh with normals and sets a light ----
  {
    const std::string jail="/tmp/pnr_litdemo_jail_"+std::to_string(::getpid());
    std::filesystem::remove_all(jail);
    pnr::loader::NativeLoader loader;
    auto lr=loader.launch(PNR_PS_LIT_DEMO_PATH,{},{"PNR_JAIL_ROOT="+jail,std::string("LD_LIBRARY_PATH=")+PNR_PSABI_DIR});
    check(lr.validated&&lr.started&&lr.finished&&!lr.signaled&&lr.exit_code==0,"lit game: ps_lit_demo launched, exit 0 (8 invalid calls refused, RDNA vs reference compared inside the game)");
    // independent CPU shading: ray-cast the cube, colour*(0.25+0.75*max(N.L,0))
    const double fc[3][2][3]={{{1,0.3,1},{0.3,1,0.3}},{{0.3,1,1},{0.3,0.3,1}},{{1,1,0.3},{1,0.3,0.3}}};  // [axis][sg]: sg0 = -axis face
    const double f=1.0/std::tan(fov*0.5);
    constexpr uint32_t N=8;bool all_ok=true;uint32_t found=0;
    double bright_lo=1e9,bright_hi=-1e9;
    for(uint32_t k=0;k<N;++k){
      char name[64];std::snprintf(name,sizeof name,"/frames/lit_%02u.ppm",k);
      std::ifstream fi(jail+name,std::ios::binary);
      if(!fi){all_ok=false;continue;}
      ++found;std::stringstream ss;ss<<fi.rdbuf();std::string d=ss.str();
      const std::string hdr="P6\n96 96\n255\n";
      if(d.size()!=hdr.size()+W*H*3){all_ok=false;continue;}
      Mat4 M=mat4_mul(mat4_rotate_x(0.5f),mat4_rotate_y(k*2.0f*(float)M_PI/N+0.3f));
      double R[3][3];for(int r=0;r<3;++r)for(int c=0;c<3;++c)R[r][c]=M[c*4+r];
      double Lv[3];
      if(k<4){Lv[0]=0.4;Lv[1]=0.6;Lv[2]=0.7;}else{Lv[0]=-0.6;Lv[1]=0.3;Lv[2]=0.7;}
      double ll=std::sqrt(Lv[0]*Lv[0]+Lv[1]*Lv[1]+Lv[2]*Lv[2]);for(double& x:Lv)x/=ll;
      uint32_t mism=0;
      for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        double nx=(x+0.5)/W*2-1,ny=(y+0.5)/H*2-1;
        double dv[3]={nx/f,-ny/f,-1.0},ov[3]={0,0,3};double o[3],dd[3];
        for(int i=0;i<3;++i){o[i]=R[0][i]*ov[0]+R[1][i]*ov[1]+R[2][i]*ov[2];dd[i]=R[0][i]*dv[0]+R[1][i]*dv[1]+R[2][i]*dv[2];}
        double tn=-1e30,tf=1e30;int ax=-1,sg=0;
        for(int i=0;i<3;++i){double t1=(-0.5-o[i])/dd[i],t2=(0.5-o[i])/dd[i];double lo=std::min(t1,t2),hi=std::max(t1,t2);if(lo>tn){tn=lo;ax=i;sg=dd[i]>0?0:1;}tf=std::min(tf,hi);}
        const bool hit=tn<tf&&tf>0;
        const unsigned char* q=reinterpret_cast<const unsigned char*>(&d[hdr.size()+(y*W+x)*3]);
        if(!hit){if(q[0]|q[1]|q[2])++mism;continue;}
        double nobj[3]={0,0,0};nobj[ax]=sg==0?-1.0:1.0;
        double nv[3];for(int r=0;r<3;++r)nv[r]=R[r][0]*nobj[0]+R[r][1]*nobj[1]+R[r][2]*nobj[2];
        double dot=nv[0]*Lv[0]+nv[1]*Lv[1]+nv[2]*Lv[2];
        double kk=0.25+0.75*std::max(dot,0.0);
        bright_lo=std::min(bright_lo,kk);bright_hi=std::max(bright_hi,kk);
        bool same=true;for(int c=0;c<3;++c)if(std::fabs(q[c]-255.0*fc[ax][sg][c]*kk)>3.5)same=false;
        if(!same)++mism;
      }
      std::printf("  lit game frame %u: CPU-shaded reference vs presented pixels: %u mismatched of %u\n",k,mism,W*H);
      if(mism>W*H/100)all_ok=false;
    }
    check(found==N,"lit game: all 8 presented PPM files exist inside the game's jail");
    check(all_ok,"lit game: presented pixels match an independent CPU Lambert shading of the cube within +-3 on 99% of pixels");
    std::printf("  lit game: shading factor range seen on visible faces %.3f..%.3f\n",bright_lo,bright_hi);
    check(bright_hi-bright_lo>0.4,"lit game: faces are visibly lit differently (shading factor spread > 0.4)");
    pnr::prx::PrxRegistry reg;pnr::loader::ImportResolution ir;
    check(pnr::loader::resolve_imports(PNR_PS_LIT_DEMO_PATH,reg,&ir)&&ir.pnr_extension==7&&ir.unknown==0,"lit game: pnrGfx* imports classify as pnr-extension");
    std::filesystem::remove_all(jail);
  }
#endif
  // ---- honest rejection paths ----
  {
    std::vector<std::vector<uint8_t>> j;std::vector<std::vector<uint32_t>> one{mat4_words(mat4_identity())};
    auto bi=idx;bi[5]=24;
    check(!vk.render_mesh_frames_rgba(32,32,vs,fs,verts,bi,true,one,black,&j).ok,"cube: out-of-range index rejected");
    auto bi2=idx;bi2.pop_back();
    check(!vk.render_mesh_frames_rgba(32,32,vs,fs,verts,bi2,true,one,black,&j).ok,"cube: index count not a multiple of 3 rejected");
    auto bv=verts;bv.push_back(0);
    check(!vk.render_mesh_frames_rgba(32,32,vs,fs,bv,idx,true,one,black,&j).ok,"cube: vertex data not a multiple of 6 floats rejected");
    check(!vk.render_mesh_frames_rgba(32,32,vs,fs,verts,idx,true,{},black,&j).ok,"cube: zero frames rejected");
    std::vector<uint32_t> bad(vs.begin(),vs.begin()+8);
    check(!vk.render_mesh_frames_rgba(32,32,bad,fs,verts,idx,true,one,black,&j).ok,"cube: truncated SPIR-V rejected by driver, no crash");
  }
}
