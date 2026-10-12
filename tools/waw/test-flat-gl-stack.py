from pathlib import Path
import argparse
import subprocess,tempfile
parser=argparse.ArgumentParser(description='Validate GL stack bounds in an explicitly supplied flat-port source tree')
parser.add_argument('port_root',type=Path)
args=parser.parse_args()
ROOT=args.port_root.resolve()
s=(ROOT/'source/dynlib.c').read_text()
begin=s.index('static int ml_gl_native_stack_bytes_remaining(');end=s.index('static ml_gl_client_array_state *ml_gl_active_texcoord_array',begin)
body=s[begin:end]
prelude=r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
typedef unsigned GLuint;
typedef struct {uint32_t size;void *stack;int32_t stackSize;} SceKernelThreadInfo;
static uintptr_t stack_low=0x80a00000;
static int32_t stack_size=0x40000;
static int query_result=0,query_calls=0,native_calls=0;
static unsigned g_gl_bad_pointer_logs;
#define ML_GL_BAD_POINTER_LOG_LIMIT 64
#define ml_warn(...) ((void)0)
static int sceKernelGetThreadInfo(int id,SceKernelThreadInfo *info){assert(id==0 && info->size==sizeof(*info));++query_calls;info->stack=(void*)stack_low;info->stackSize=stack_size;return query_result;}
static int ml_native_guest_stack_bounds(uintptr_t p,uintptr_t *lo,uintptr_t *hi){++native_calls;if(p<0x80a00000 || p>=0x80a40000)return 0;*lo=0x80a00000;*hi=0x80a40000;return 1;}
static int ml_mem_backend_tracked_bytes_remaining(const void *p,size_t *n){uintptr_t a=(uintptr_t)p;if(a>=0x90000000 && a<0x90001000){*n=0x90001000-a;return 1;}return 0;}
static int ml_macho_active_pointer_is_image_address(uintptr_t p){return p>=0x4000 && p<0x8000;}
static void *ml_macho_translate_active_pointer(uintptr_t p,size_t n,size_t *remaining){if(ml_macho_active_pointer_is_image_address(p)){*remaining=0x8000-p;if(n<=*remaining)return (void*)p;}*remaining=0;return NULL;}
static int ml_gl_pointer_can_be_buffer_offset(const void *p){return (uintptr_t)p<0x10000;}
"""
tests=r"""
static unsigned cases;
static void check(uintptr_t address,size_t n,int offset,GLuint binding,int expected){int valid=-1;const void *p=ml_gl_guest_pointer_or_null("_glShaderSource.strings",(void*)address,n,offset,binding,&valid);assert(valid==expected);assert(expected?p==(void*)address:p==NULL);++cases;}
int main(void){
    for(uintptr_t p=0x80a318b8;p<=0x80a318e4;p+=4)check(p,4,0,0,!ML_ENABLE_WAW_INTEGRATION);
    check(0x80a3fffc,4,0,0,!ML_ENABLE_WAW_INTEGRATION);
    check(0x80a3fffc,5,0,0,0);check(0x80a40000,1,0,0,0);check(0x809fffff,1,0,0,0);
    check(0x90000000,4096,0,0,1);check(0x90000ffc,4,0,0,1);check(0x90000ffc,5,0,0,0);
    check(0x4000,16,0,0,1);check(0x7ffc,5,0,0,0);
    check(0,4,0,0,1);check(0xdeadbeef,0,0,0,1);
    check(0x123,4,1,12,1);check(0x123,4,0,12,0);
    if(!ML_ENABLE_WAW_INTEGRATION && !ML_ENABLE_TTBR_REMAP_NATIVE_RUNNER){
        int calls=query_calls;check(0x80a318e4,4,1,12,1);assert(query_calls==calls);
        query_result=-1;check(0x80a318e4,4,0,0,0);query_result=0;
        stack_low=0;check(0x80a318e4,4,0,0,0);
        stack_low=0x80a00000;stack_size=0;check(0x80a318e4,4,0,0,0);
        stack_low=0x1000;stack_size=-8192;check(0x80a318e4,4,0,0,0);
        stack_low=UINTPTR_MAX-16;stack_size=32;check(UINTPTR_MAX-8,4,0,0,0);
        assert(query_calls>0 && native_calls==0);
    }
    else if(ML_ENABLE_TTBR_REMAP_NATIVE_RUNNER && !ML_ENABLE_WAW_INTEGRATION)assert(native_calls>0 && query_calls==0);
    else assert(native_calls==0 && query_calls==0);
    printf("%u pointer/range cases pass: TTBR=%d WAW=%d\n",cases,ML_ENABLE_TTBR_REMAP_NATIVE_RUNNER,ML_ENABLE_WAW_INTEGRATION);
}
"""
with tempfile.TemporaryDirectory(prefix='flat-gl-stack-') as folder:
    temp=Path(folder);(temp/'test.c').write_text(prelude+body+tests)
    for ttbr,waw in [(0,0),(1,0),(0,1),(1,1)]:
        subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',f'-DML_ENABLE_TTBR_REMAP_NATIVE_RUNNER={ttbr}',f'-DML_ENABLE_WAW_INTEGRATION={waw}',str(temp/'test.c'),'-o',str(temp/'test')],check=True)
        subprocess.run([str(temp/'test')],check=True)
