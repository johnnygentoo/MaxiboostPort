/* BB_IMPORT_TRACE=<prefix>[,<prefix>...]: every call of an import whose name starts with one of
 * the prefixes (e.g. sceNp,_ZN3sce7Toolkit,sceUserService) is logged with its first six integer
 * arguments, its return value and the guest code that called it:
 *   Import: sceNpGetState(0x1, 0x2fe0c4, ...) <- guest+0x5c1a3c
 *   Import: sceNpGetState -> 0
 * The import is bound to a thunk instead of its function. The thunk keeps the call's stack as the
 * caller left it (arguments passed on the stack stay where the function expects them): it saves
 * the argument registers (integer and vector), logs, replaces the return address with an exit
 * trampoline (the real one kept on a per-thread stack) and jumps to the function; the trampoline
 * logs the result and returns to the real address. BB_IMPORT_TRACE_SKIP=<name>,... leaves out
 * functions called too often to read; BB_IMPORT_TRACE=* traces every import.
 * BB_IMPORT_TRACE_GATE=<file>: calls are logged only while that file exists (a moment of a long
 * run, e.g. one menu choice with BB_IMPORT_TRACE=*). */
#define _GNU_SOURCE
#include "runtime.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_TRACED 4096
#define THUNK_SIZE 192
#define DEPTH 256

static const char *names[MAX_TRACED];
static unsigned traced;
static unsigned char *code;
static uintptr_t exit_trampoline;
static const unsigned char *image_base;
static _Thread_local struct { uintptr_t ret; unsigned index; } frames[DEPTH];
static _Thread_local unsigned depth;

static int listed(const char *list, const char *symbol, int whole) {
    for (const char *at=list; at && *at;) {
        const char *end=strchr(at,',');
        const size_t n=end ? (size_t)(end-at) : strlen(at);
        if (n==1 && *at=='*' && !whole) return 1; /* every import */
        if (n && !strncmp(symbol,at,n) && (!whole || !symbol[n])) return 1;
        at=end ? end+1 : NULL;
    }
    return 0;
}

static volatile int gate_open=1;
static void *gate_watch(void *path) {
    for (;;) {
        gate_open=access(path,F_OK)==0;
        struct timespec t={0,100000000};
        nanosleep(&t,NULL);
    }
    return NULL;
}

/* regs: rdi rsi rdx rcx r8 r9 as pushed (r9 lowest); ret: the return address's slot. */
__attribute__((used)) void runtime_trace_enter(uint64_t index, const uint64_t *regs, uintptr_t *ret) {
    const uintptr_t caller=*ret;
    if (gate_open) printf("Import: %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") <- guest+%#" PRIxPTR "\n",
           names[index],regs[5],regs[4],regs[3],regs[2],regs[1],regs[0],caller-(uintptr_t)image_base);
    if (gate_open) fflush(stdout);
    if (depth<DEPTH) {
        frames[depth].ret=caller; frames[depth].index=(unsigned)index; ++depth;
        *ret=exit_trampoline;
    }
}
__attribute__((used)) uintptr_t runtime_trace_exit(uint64_t value) {
    --depth;
    if (gate_open) { printf("Import: %s -> %#" PRIx64 "\n",names[frames[depth].index],value); fflush(stdout); }
    return frames[depth].ret;
}

/* Exit trampoline: rax/rdx and xmm0/xmm1 (the return registers) survive the logging. */
__asm__(".text\n"
        ".globl runtime_trace_exit_stub\n"
        "runtime_trace_exit_stub:\n"
        "    subq $56,%rsp\n"           /* rsp was 16-aligned at the function's ret: stays so */
        "    movq %rax,(%rsp)\n"
        "    movq %rdx,8(%rsp)\n"
        "    movdqu %xmm0,16(%rsp)\n"
        "    movdqu %xmm1,32(%rsp)\n"
        "    movq %rax,%rdi\n"
        "    subq $8,%rsp\n"
        "    call runtime_trace_exit\n"
        "    addq $8,%rsp\n"
        "    movq %rax,%r11\n"
        "    movq (%rsp),%rax\n"
        "    movq 8(%rsp),%rdx\n"
        "    movdqu 16(%rsp),%xmm0\n"
        "    movdqu 32(%rsp),%xmm1\n"
        "    addq $56,%rsp\n"
        "    jmp *%r11\n");
void runtime_trace_exit_stub(void);

static void emit(unsigned char **p, const char *bytes, size_t n) { memcpy(*p,bytes,n); *p+=n; }
static void emit64(unsigned char **p, uint64_t v) { memcpy(*p,&v,8); *p+=8; }

void runtime_trace_set_image(const unsigned char *image) { image_base=image; }

uintptr_t runtime_trace_wrap(const char *symbol, uintptr_t target) {
    static const char *list, *skip; static int read;
    if (!read) {
        list=getenv("BB_IMPORT_TRACE"); skip=getenv("BB_IMPORT_TRACE_SKIP"); read=1;
        const char *gate=getenv("BB_IMPORT_TRACE_GATE");
        pthread_t watcher;
        if (list && *list && gate && *gate) { gate_open=0; if (!pthread_create(&watcher,NULL,gate_watch,(void *)gate)) pthread_detach(watcher); }
    }
    /* Calls that do not come back where they were called (another fiber, the end of a thread) would
     * leave the exit trampoline's stack wrong: never traced. */
    static const char never[]="sceFiberRun,sceFiberSwitch,sceFiberReturnToThread,scePthreadExit,pthread_exit,_exit,exit";
    if (!list || !*list || !symbol || !target || !listed(list,symbol,0) || listed(skip,symbol,1) ||
        listed(never,symbol,1)) return target;
    for (unsigned i=0;i<traced;++i) if (!strcmp(names[i],symbol)) return (uintptr_t)(code+i*THUNK_SIZE);
    if (traced==MAX_TRACED) return target;
    if (!code) {
        code=mmap(NULL,(size_t)MAX_TRACED*THUNK_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (code==MAP_FAILED) { code=NULL; return target; }
        exit_trampoline=(uintptr_t)runtime_trace_exit_stub;
    }
    const unsigned index=traced++;
    names[index]=symbol;
    unsigned char *p=code+index*THUNK_SIZE;
    /* At entry rsp = 8 mod 16. push rdi rsi rdx rcx r8 r9 (48), xmm0-7 (128): rsp = 8 mod 16,
     * one more 8 for the call. */
    emit(&p,"\x57\x56\x52\x51\x41\x50\x41\x51",8);         /* push rdi,rsi,rdx,rcx,r8,r9 */
    emit(&p,"\x48\x81\xec\x88\x00\x00\x00",7);             /* sub rsp,136 */
    for (int x=0;x<8;++x) {                                  /* movdqu [rsp+8+16x],xmmx */
        const unsigned char op[]={0xf3,0x0f,0x7f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(8+16*x)};
        emit(&p,(const char *)op,6);
    }
    emit(&p,"\x48\xbf",2); emit64(&p,index);                 /* mov rdi,index */
    emit(&p,"\x48\x8d\xb4\x24\x88\x00\x00\x00",8);         /* lea rsi,[rsp+136]: the saved registers */
    emit(&p,"\x48\x8d\x94\x24\xb8\x00\x00\x00",8);         /* lea rdx,[rsp+184]: the return address */
    emit(&p,"\x48\xb8",2); emit64(&p,(uint64_t)(uintptr_t)runtime_trace_enter);
    emit(&p,"\xff\xd0",2);                                   /* call rax */
    for (int x=0;x<8;++x) {                                  /* movdqu xmmx,[rsp+8+16x] */
        const unsigned char op[]={0xf3,0x0f,0x6f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(8+16*x)};
        emit(&p,(const char *)op,6);
    }
    emit(&p,"\x48\x81\xc4\x88\x00\x00\x00",7);             /* add rsp,136 */
    emit(&p,"\x41\x59\x41\x58\x59\x5a\x5e\x5f",8);         /* pop r9,r8,rcx,rdx,rsi,rdi */
    emit(&p,"\x48\xb8",2); emit64(&p,(uint64_t)target);      /* mov rax,target (rax: al = vector args count, preserved enough: set below) */
    emit(&p,"\xff\xe0",2);                                   /* jmp rax */
    if (p-(code+index*THUNK_SIZE)>THUNK_SIZE) { fputs("STOP: trace thunk too large\n",stderr); exit(25); }
    return (uintptr_t)(code+index*THUNK_SIZE);
}
