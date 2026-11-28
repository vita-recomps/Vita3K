#include "frame_pacing.h"
#include <cstdint>
#include <map>
#include <vector>
#include <stdexcept>
#include <iostream>
using Address=uint32_t;
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct MemState{std::map<Address,std::vector<uint8_t>> blocks;void add(Address a,size_t n){blocks[a]=std::vector<uint8_t>(n);}void* at(Address a){for(auto& [b,v]:blocks)if(a>=b&&uint64_t(a)<uint64_t(b)+v.size())return v.data()+(a-b);return nullptr;}};
bool is_valid_addr(MemState&m,Address a){return m.at(a)!=nullptr;}
template<class T>struct Ptr{Address address;explicit Ptr(Address a):address(a){}T*get(MemState&m){return static_cast<T*>(m.at(address));}};
struct EmuEnvState{MemState mem;};bool loadingFix=true;bool aot_module_has_feature(const char*,const char*){return loadingFix;}
using ss_delta::frame_pacing::frame_period_us;
int update_frame_rate(EmuEnvState &emuenv, int fps, int current) {
    if(!ss_delta::frame_pacing::supported(fps))return -1;
    auto &mem = emuenv.mem;
    const auto u32 = [&](Address a) -> uint32_t * { return is_valid_addr(mem, a) ? Ptr<uint32_t>(a).get(mem) : nullptr; };
    uint32_t *const holder = u32(0x81c957cc);
    if (!holder || !*holder)
        return 0;
    uint32_t *const timer = u32(*holder);
    uint32_t *const flip = u32(0x81d3a630);
    if (!timer || !*timer || !is_valid_addr(mem, *timer + 7) || !flip)
        return 0;
    uint64_t *const period = Ptr<uint64_t>(*timer).get(mem);
    const uint32_t *const loading_task = u32(0x81c9603c);
    const uint32_t *const loading_state = loading_task && *loading_task ? u32(*loading_task + 0xa4) : nullptr;
    const uint32_t state = loading_state ? *loading_state : 0;
    const bool loading = state == 2 || state == 3 || state == 5;
    // asked when a loading screen starts: the eboot's native code is checked by then
    const bool loading_fixed = loading && aot_module_has_feature("eboot", "loading_race");
    static bool told = false;
    if (loading && !told) {
        told = true;
        LOG_INFO("frame_rate: loading screens {}", loading_fixed ? "at the set rate (the recompiled eboot has the fix)" : "at 30 fps (an eboot without the loading-screen fix)");
    }
    const int want = loading && !loading_fixed ? 30 : fps;
    if (want == current && *period == frame_period_us(want) && *flip == ss_delta::frame_pacing::flip_interval(want))
        return current;
    if (!ss_delta::frame_pacing::known_period(*period) || (*flip != 2 && *flip != 1)) {
        LOG_WARN("frame_rate: unexpected frame timer {} us / flip threshold {}; left alone", *period, *flip);
        return -1;
    }
    *period = frame_period_us(want);
    *flip = ss_delta::frame_pacing::flip_interval(want);
    LOG_INFO("frame_rate: {} fps{} (loading state {}, frame timer {} us, flip every {} vblank)", want, want != fps ? " for a loading screen" : "", state, *period, *flip);
    return want;
}
void check(bool b,const char*s){if(!b)throw std::runtime_error(s);}
int main(){EmuEnvState e;e.mem.add(0x81c957cc,4);e.mem.add(0x1000,4);e.mem.add(0x2000,8);e.mem.add(0x81d3a630,4);e.mem.add(0x81c9603c,4);e.mem.add(0x3000,0xa8);
 *Ptr<uint32_t>(0x81c957cc).get(e.mem)=0x1000;*Ptr<uint32_t>(0x1000).get(e.mem)=0x2000;*Ptr<uint32_t>(0x81c9603c).get(e.mem)=0x3000;
 auto& period=*Ptr<uint64_t>(0x2000).get(e.mem);auto& flip=*Ptr<uint32_t>(0x81d3a630).get(e.mem);auto& state=*Ptr<uint32_t>(0x30a4).get(e.mem);period=33333;flip=2;int current=30;int tests=0;
 for(int pass=0;pass<100;pass++)for(int target:{60,45,40,30,24,40,45,24,60,30}){current=update_frame_rate(e,target,current);check(current==target,"target not applied");check(period==frame_period_us(target),"wrong timer");check(flip==ss_delta::frame_pacing::flip_interval(target),"wrong flip");tests++;}
 state=2;loadingFix=false;current=update_frame_rate(e,45,current);check(current==30&&period==33333&&flip==2,"loading fallback");state=0;current=update_frame_rate(e,45,current);check(current==45&&period==22222,"post-loading restore");
 loadingFix=true;state=2;current=update_frame_rate(e,45,current);check(current==45,"patched loader must allow target");state=0;
 period=12345;auto before=period;check(update_frame_rate(e,45,current)==-1&&period==before,"unknown timer overwritten");period=22222;flip=3;check(update_frame_rate(e,45,current)==-1&&flip==3,"unknown flip overwritten");check(update_frame_rate(e,0,current)==-1,"zero target");check(update_frame_rate(e,90,current)==-1,"invalid target");
 *Ptr<uint32_t>(0x81c957cc).get(e.mem)=0;check(update_frame_rate(e,45,current)==0,"uninitialized timer");
 std::cout<<"PASS: actual update_frame_rate method, "<<tests<<" live transitions, loading fallback, unknown-memory guards and invalid targets\n";}
