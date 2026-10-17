/* Process and module queries of a retail PS4 (GUNDAM VERSUS asks them; Bloodborne and GODZILLA did
 * not):
 *   - a base PS4, or a PS4 Pro with BB_NEO=1 (sceKernelIsNeoMode), six game cores;
 *   - modules the game loads at run time from its own sce_module folder were linked into the
 *     image beforehand (scripts/link_modules.py) or are served by the runtime (libSceNpToolkit2),
 *     so loading one only hands out a handle; system modules are not there;
 *   - no unwind tables are offered (C++ exceptions would need them: logged when asked);
 *   - local time from the host's time zone. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KERNEL_ENOENT ((int32_t)0x80020002)
#define KERNEL_ESRCH ((int32_t)0x80020003)
#define KERNEL_EFAULT ((int32_t)0x8002000e)
#define KERNEL_EINVAL ((int32_t)0x80020016)

/* BB_NEO=1: a PS4 Pro (the game's Pro mode). GUNDAM VERSUS renders its scene at 1440x1080 on a
 * base PS4 and at 1920x1080 on a Pro. The GPU library asks the same (runtime_neo_mode). */
int runtime_neo_mode(void) {
    static int neo=-1;
    if (neo<0) { const char *v=getenv("BB_NEO"); neo=v && v[0]=='1'; if (neo) puts("Runtime: PS4 Pro mode (BB_NEO=1)"); }
    return neo;
}
static ABI int32_t is_neo_mode(void) { return runtime_neo_mode(); }
static ABI int32_t cpu_mode(void) { return 0; } /* six cores for the game */
static ABI uint64_t get_gpi(void) { return 0; }
static ABI int32_t set_gpo(uint32_t value) { (void)value; return 0; }

/* Run-time module loading: a handle for the game's own modules, ENOENT for others. */
static _Atomic int32_t next_handle=0x100;
static ABI int32_t load_start_module(const char *path, uint64_t args, const void *argp, uint32_t flags,
                                     const void *option, int32_t *result) {
    (void)args; (void)argp; (void)option;
    const char *base=path ? strrchr(path,'/') : NULL;
    base=base ? base+1 : path;
    const int own=path && (strstr(path,"/app0/") || !strchr(path,'/')) && base && strstr(base,".prx");
    printf("Runtime: sceKernelLoadStartModule(%s, flags %#x): %s\n",path ? path : "(null)",flags,
           own ? "the game's own module, already linked or served" : "not present (ENOENT)");
    if (!own) return KERNEL_ENOENT;
    if (result) *result=0;
    return atomic_fetch_add(&next_handle,1);
}
static ABI int32_t stop_unload_module(int32_t handle, uint64_t args, const void *argp, uint32_t flags,
                                      const void *option, int32_t *result) {
    (void)handle; (void)args; (void)argp; (void)flags; (void)option;
    if (result) *result=0;
    return 0;
}
static ABI int32_t dlsym(int32_t handle, const char *symbol, void **address) {
    printf("Runtime: sceKernelDlsym(%d, %s): not found\n",handle,symbol ? symbol : "(null)");
    if (address) *address=NULL;
    return KERNEL_ESRCH;
}

/* Module information by address: libc's unwinder (C++ exceptions) and crash reports ask. */
static void unwind_asked(const char *what, uint64_t address) {
    static _Atomic int told;
    if (atomic_fetch_add(&told,1)<8)
        printf("Runtime: %s(%#llx): no module information (C++ exception or backtrace)\n",what,(unsigned long long)address);
}
static ABI int32_t module_info_from_addr(uint64_t address, int32_t flags, void *info) {
    (void)flags;
    if (!info) return KERNEL_EFAULT;
    unwind_asked("sceKernelGetModuleInfoFromAddr",address);
    return KERNEL_ESRCH;
}
static ABI int32_t module_info_for_unwind(uint64_t address, int32_t flags, void *info) {
    (void)flags;
    if (!info) return KERNEL_EFAULT;
    unwind_asked("sceKernelGetModuleInfoForUnwind",address);
    return KERNEL_ESRCH;
}
static ABI int32_t module_segment_info(uint64_t address, void *info) {
    (void)info;
    unwind_asked("sceKernelInternalMemoryGetModuleSegmentInfo",address);
    return KERNEL_ESRCH;
}
static ABI int32_t phdr_match_addr(const void *info, uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t is_signal_return(const void *address) { (void)address; return 0; }
static ABI int32_t guest_sigreturn(const void *context) {
    (void)context;
    fputs("STOP: sigreturn without a guest signal frame\n",stderr);
    exit(24);
}
static ABI void cxa_finalize(void *dso) { (void)dso; } /* atexit handlers run at the game's exit() */

/* Local time: the host's zone. A Timesec is the time, the zone's offset east of UTC and the
 * daylight saving offset, in seconds. */
typedef struct { int64_t time; uint32_t west_sec, dst_sec; } Timesec;
typedef struct { int32_t minutes_west, dst_time; } Timezone;
static void zone(int64_t utc, int32_t *offset, int32_t *dst) {
    time_t t=(time_t)utc; struct tm local;
    localtime_r(&t,&local);
    *dst=local.tm_isdst>0 ? 3600 : 0;
    *offset=(int32_t)local.tm_gmtoff-*dst;
}
static ABI int32_t utc_to_local(int64_t utc, int64_t *local, Timesec *st, uint64_t *dst_seconds) {
    int32_t offset, dst;
    zone(utc,&offset,&dst);
    if (local) *local=utc+offset+dst;
    if (st) { st->time=utc; st->west_sec=(uint32_t)offset; st->dst_sec=(uint32_t)dst; }
    if (dst_seconds) *dst_seconds=(uint64_t)dst;
    return 0;
}
static ABI int32_t local_to_utc(int64_t local, int64_t unused, int64_t *utc, Timezone *tz, int32_t *dst_seconds) {
    (void)unused;
    if (!tz) return KERNEL_EINVAL;
    int32_t offset, dst;
    zone(local,&offset,&dst);
    tz->minutes_west=-offset/60; tz->dst_time=dst/60;
    if (utc) *utc=local-offset-dst;
    if (dst_seconds) *dst_seconds=dst;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceKernelIsNeoMode",is_neo_mode}, {"sceKernelGetCpumode",cpu_mode},
    {"sceKernelGetGPI",get_gpi}, {"sceKernelSetGPO",set_gpo},
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"sceKernelDlsym",dlsym},
    {"sceKernelGetModuleInfoFromAddr",module_info_from_addr},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",module_segment_info},
    {"__elf_phdr_match_addr",phdr_match_addr}, {"_is_signal_return",is_signal_return},
    {"sigreturn",guest_sigreturn}, {"__pthread_cxa_finalize",cxa_finalize},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
};

uintptr_t runtime_process_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }

/* __progname: the program's name, read by libc's messages. */
static const char *progname="eboot.bin";
uintptr_t runtime_process_resolve_data(const char *name) {
    const char *symbol=runtime_symbol(name);
    return symbol && !strcmp(symbol,"__progname") ? (uintptr_t)&progname : 0;
}
