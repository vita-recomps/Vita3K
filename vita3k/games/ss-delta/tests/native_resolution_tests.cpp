#include "native_resolution.h"
#include <array>
#include <vector>
#include <limits>
#include <stdexcept>
#include <iostream>
#include <cmath>
using namespace ss_delta::native_resolution;
void check(bool b,const char*m){if(!b)throw std::runtime_error(m);}void word(std::vector<uint8_t>&b,size_t n,uint32_t v){std::memcpy(b.data()+n,&v,4);}uint32_t word(const std::vector<uint8_t>&b,size_t n){uint32_t v;std::memcpy(&v,b.data()+n,4);return v;}
int main(){check(setting_mode("true")==0&&setting_mode("false")==1&&setting_mode("half")==3,"legacy choices");std::array<Mode,4> expected={Mode{960,544},Mode{720,408},Mode{640,368},Mode{480,272}};
 for(int i=0;i<4;i++){auto m=mode_for(i);check(m.width==expected[i].width&&m.height==expected[i].height,"native dimensions");check(m.padded_width()%32==0&&m.padded_height()%32==0,"padding");check(m.uv_x()>0&&m.uv_x()<=1&&m.uv_y()>0&&m.uv_y()<=1,"UV domain");std::vector<uint8_t> bytes(160,0x7b);word(bytes,4,0x3f7a6f4e);word(bytes,12,0x3f7b13b2);word(bytes,52,0x3f7a6f4e);word(bytes,60,0x3f7b13b2);auto before=bytes;auto n=patch_literals(bytes.data(),bytes.size(),32,8,m);check(n==(i==1?0:1),"recognized literal pair");for(size_t j=0;j<bytes.size();j++)if(!((j>=52&&j<56)||(j>=60&&j<64)))check(bytes[j]==before[j],"changed outside literal pair");float x,y;auto xb=word(bytes,52),yb=word(bytes,60);std::memcpy(&x,&xb,4);std::memcpy(&y,&yb,4);check(std::abs(x-m.uv_x())<.000001&&std::abs(y-m.uv_y())<.000001,"wrong UV ratio");check(patch_literals(bytes.data(),bytes.size(),32,8,m)==0,"not idempotent");}
 check(mode_for(4).width==960&&mode_for(99).height==544,"game fallback mode");check(std::abs(mode_for(3).uv_y()-272.f/288)<.000001,"half-depth padding");
 size_t cases=0;for(size_t size=0;size<=128;size++)for(size_t offset=0;offset<=140;offset++)for(size_t count:{size_t(0),size_t(1),size_t(2),size_t(8),std::numeric_limits<size_t>::max()}){std::vector<uint8_t> data(192,0x55);auto old=data;check(patch_literals(data.data(),size,offset,count,mode_for(3))==0,"invalid or unmatched data changed");check(data==old,"bounds violation");cases++;}
 check(patch_literals(nullptr,128,0,8,mode_for(3))==0,"null input");std::cout<<"PASS native modes, legacy settings, per-mode depth UVs, table-only changes, idempotency and "<<cases<<" bounds cases\n";
}
