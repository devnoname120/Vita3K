from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'vita3k/cpu/src/dynarmic_cpu.cpp').read_text()
parts=[]
for begin,end in [('    template <typename T>\n    T MemoryRead(', '    uint8_t MemoryRead8('),('    template <typename T>\n    void MemoryWrite(', '    void MemoryWrite8('),('    template <typename T>\n    bool MemoryWriteExclusive(', '    bool MemoryWriteExclusive8(')]:
    start=s.index(begin);parts.append(s[start:s.index(end,start)])
program=r'''
#include <mem/ptr.h>
#include <mem/state.h>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#define LOG_ERROR(...) do {} while(0)
#define LOG_CRITICAL(...) do {} while(0)
#define LOG_TRACE(...) do {} while(0)
namespace Dynarmic::A32 { using VAddr=uint32_t; }
struct Parent { MemState *mem; };
struct FakeCPU { bool log_mem=false; uint32_t get_pc() { return 0x2000; } };
struct Callback { Parent *parent; FakeCPU *cpu;
'''+''.join(parts)+r'''
};
static unsigned checks;
template<class T> void run(Callback &cb, MemState &mem) {
    for(uint32_t addr : {0x1000u,0x2040u,0x3ff8u,0x4000u}) {
        auto *p=Ptr<T>(addr).get(mem);*p=T(0x125371);
        assert(cb.MemoryRead<T>(addr)==T(0x125371));
        cb.MemoryWrite<T>(addr,T(0x5472));assert(*p==T(0x5472));
        assert(!cb.MemoryWriteExclusive<T>(addr,T(0x8453),T(0x8888)));assert(*p==T(0x5472));
        assert(cb.MemoryWriteExclusive<T>(addr,T(0x8453),T(0x5472)));assert(*p==T(0x8453));
        ++checks;
    }
    for(uint32_t addr : {0u,8u,0xff8u,0x6000u}) {
        assert(cb.MemoryRead<T>(addr)==0);
        cb.MemoryWrite<T>(addr,T(0x7153));
        assert(!cb.MemoryWriteExclusive<T>(addr,T(0x4321),T(0)));
        ++checks;
    }
}
int main() {
    MemState mem;assert(init(mem,true));assert(try_alloc_at(mem,0x1000,0x4000,"low-callbacks")==0x1000);
    Parent parent{&mem};FakeCPU cpu;Callback cb{&parent,&cpu};
    run<uint8_t>(cb,mem);run<uint16_t>(cb,mem);run<uint32_t>(cb,mem);run<uint64_t>(cb,mem);
    std::printf("%u extracted read/write/exclusive cases pass with actual memory/Ptr, host page %d\n",checks,mem.host_page_size);
    free(mem,0x1000);
}
'''
out=ROOT/'build/waw-checks';(out/'low-callbacks-test.cpp').write_text(program)
cmd=['clang++','-std=c++23','-O1','-pthread','-DSPDLOG_FMT_EXTERNAL','-DSPDLOG_COMPILED_LIB','-DSPDLOG_NO_THREAD_ID']
for name in ('vita3k/mem/include','vita3k/util/include','external/boost','external/fmt/include','external/spdlog/include'):cmd+=['-I',str(ROOT/name)]
cmd+=[str(out/'low-callbacks-test.cpp'),str(ROOT/'vita3k/mem/src/allocator.cpp'),str(ROOT/'vita3k/mem/src/mem.cpp')]
for name in ('vita3k/util/RelWithDebInfo/libutil.a','external/boost/lib/libboost_filesystem.a','external/boost/lib/libboost_atomic.a','external/spdlog/RelWithDebInfo/libspdlog.a','external/fmt/RelWithDebInfo/libfmt.a','external/RelWithDebInfo/libtracy.a'):cmd.append(str(ROOT/'build/macos-xcode'/name))
cmd+=['-o',str(out/'low-callbacks-test')];subprocess.run(cmd,check=True);subprocess.run([str(out/'low-callbacks-test')],check=True)
