from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'vita3k/sdl-frontend/src/session.cpp').read_text()
start=source.index('        const char *capture_setting')
init=source[start:source.index('        bool running',start)]
start=source.index('            if (running && capture_frames')
step=source[start:source.index('\n\n',start)]
program=r"""#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
static uint64_t tick;
static unsigned calls;
static uint64_t SDL_GetTicks(){return tick;}
static void take_screenshot(int){++calls;}
static void run(const char *setting,const char *id,bool active){
    if(setting)setenv("VITA3K_WAW_CAPTURE",setting,1);else unsetenv("VITA3K_WAW_CAPTURE");
    struct {std::string app_path;} launch_request{id};
    tick=0;calls=0;int emuenv=0;bool running=active;
"""+init+'auto step=[&](){'+step+r"""};
    const bool expected=active && (std::string(id)=="WAW000001" || std::string(id)=="BIOS00001") && setting && std::string(setting)=="1";
    tick=4999;step();assert(calls==0);
    tick=5000;step();assert(calls==(expected?1u:0u));
    step();assert(calls==(expected?1u:0u));
    tick=9999;step();assert(calls==(expected?1u:0u));
    for(unsigned i=2;i<100;++i){tick=i*5000;step();}
    assert(calls==(expected?24u:0u));
}
int main(){
    for(const char *s:{static_cast<const char *>(nullptr),"0","1","yes","11"})
        for(const char *id:{"WAW000001","BIOS00001","OTHER0001"})
            for(bool r:{false,true})run(s,id,r);
    std::puts("PASS: 30 actual-source capture scheduler cases");
}
"""
cpp=ROOT/'build/waw-checks/capture-schedule-current.cpp';cpp.write_text(program)
exe=cpp.with_suffix('')
subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
