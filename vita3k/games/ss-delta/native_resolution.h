// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string_view>
namespace ss_delta::native_resolution {
struct Mode {
    uint32_t width,height;
    constexpr uint32_t padded_width()const{return (width+31u)&~31u;}
    constexpr uint32_t padded_height()const{return (height+31u)&~31u;}
    constexpr float uv_x()const{return float(width)/padded_width();}
    constexpr float uv_y()const{return float(height)/padded_height();}
};
constexpr Mode mode_for(int mode){return mode==1?Mode{720,408}:mode==2?Mode{640,368}:mode==3?Mode{480,272}:Mode{960,544};}
// Keep legacy true/false values as valid choices, so existing settings keep their old resolution.
constexpr int setting_mode(std::string_view value){return value=="half"?3:value=="false"?1:0;}
inline size_t patch_literals(uint8_t*bytes,size_t size,size_t table,size_t count,Mode mode){
    if(!bytes||table>size||count>(size-table)/8||count<2)return 0;
    // Preserve original retail literals exactly (the stored Y ratio differs by one ULP).
    if(mode.width==720&&mode.height==408)return 0;
    constexpr uint32_t original_x=0x3f7a6f4e,original_y=0x3f7b13b2;
    float xf=mode.uv_x(),yf=mode.uv_y();uint32_t xb,yb;std::memcpy(&xb,&xf,4);std::memcpy(&yb,&yf,4);
    if(xb==original_x&&yb==original_y)return 0;
    size_t changed=0;
    for(size_t i=0;i+1<count;i++){
        const size_t x=table+i*8+4,y=x+8;uint32_t before_x,before_y;
        std::memcpy(&before_x,bytes+x,4);std::memcpy(&before_y,bytes+y,4);
        if(before_x==original_x&&before_y==original_y){std::memcpy(bytes+x,&xb,4);std::memcpy(bytes+y,&yb,4);changed++;}
    }
    return changed;
}
}
