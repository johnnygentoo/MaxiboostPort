/* BB_PEEK_FILE=<path>: reads of the running game's memory for debugging a hang (the kernel's
 * ptrace restrictions keep other processes out of /proc/<pid>/mem). A thread looks at the file
 * every 250 ms; each line is a request, answered in the log, then the file is removed:
 *   q ADDR [OFF ...]   a 64-bit value at ADDR, then at value+OFF for each OFF (a pointer chain)
 *   d ADDR             a 32-bit value
 *   x ADDR [LEN]       up to 256 bytes in hex
 *   m ADDR             the guest mutex handle at ADDR: its host mutex's lock word, count and owner
 *                      thread (with the thread's name)
 * Addresses are hexadecimal; a read of unmapped memory says so instead of faulting. */
#define _GNU_SOURCE
#include "runtime.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Memory the GPU layer has read-protected (to see the GPU's writes first) is read through the
 * pool's second view, as the protection leaves it. */
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
static int peek(uint64_t address, void *out, size_t size) {
    struct iovec local={out,size}, remote={(void *)(uintptr_t)address,size};
    if (process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)size) return 1;
    if (!runtime_memory_is_mapped(address,size)) return 0;
    runtime_memory_read_backing(address,out,size);
    printf("Peek: %#" PRIx64 " is protected; read through the backing view\n",address);
    return 1;
}

static void thread_name(int tid, char *name, size_t size) {
    char path[64];
    snprintf(path,sizeof(path),"/proc/self/task/%d/comm",tid);
    FILE *f=fopen(path,"r");
    name[0]=0;
    if (f) { if (fgets(name,(int)size,f)) name[strcspn(name,"\n")]=0; fclose(f); }
}

static void answer(char *line) {
    char *save=NULL, *op=strtok_r(line," \t\n",&save), *arg=strtok_r(NULL," \t\n",&save);
    if (!op || !arg) return;
    uint64_t address=strtoull(arg,NULL,16);
    if (!strcmp(op,"q")) {
        for (;;) {
            uint64_t value;
            if (!peek(address,&value,8)) { printf("Peek: q %#" PRIx64 ": unmapped\n",address); return; }
            printf("Peek: q %#" PRIx64 " = %#" PRIx64 "\n",address,value);
            char *off=strtok_r(NULL," \t\n",&save);
            if (!off) return;
            address=value+strtoull(off,NULL,16);
        }
    } else if (!strcmp(op,"d")) {
        uint32_t value;
        if (peek(address,&value,4)) printf("Peek: d %#" PRIx64 " = %#x (%u)\n",address,value,value);
        else printf("Peek: d %#" PRIx64 ": unmapped\n",address);
    } else if (!strcmp(op,"x")) {
        char *n=strtok_r(NULL," \t\n",&save);
        size_t size=n ? strtoull(n,NULL,0) : 64;
        if (size>256) size=256;
        unsigned char bytes[256];
        if (!peek(address,bytes,size)) { printf("Peek: x %#" PRIx64 ": unmapped\n",address); return; }
        for (size_t i=0;i<size;i+=16) {
            printf("Peek: %#" PRIx64 ":",address+i);
            for (size_t j=i;j<i+16 && j<size;++j) printf(" %02x",bytes[j]);
            printf("\n");
        }
    } else if (!strcmp(op,"m")) {
        uint64_t handle;
        int words[3];
        if (!peek(address,&handle,8) || handle<16 || !peek(handle,words,sizeof(words))) {
            printf("Peek: m %#" PRIx64 ": no mutex there\n",address); return;
        }
        char name[32];
        thread_name(words[2],name,sizeof(name));
        printf("Peek: mutex %#" PRIx64 " (handle %#" PRIx64 "): lock %d, count %d, owner %d %s\n",
               address,handle,words[0],words[1],words[2],name);
    }
}

static void *watch(void *path) {
    for (;;) {
        FILE *f=fopen(path,"r");
        if (f) {
            char line[512];
            while (fgets(line,sizeof(line),f)) answer(line);
            fclose(f);
            remove(path);
            fflush(stdout);
        }
        struct timespec t={0,250000000};
        nanosleep(&t,NULL);
    }
    return NULL;
}

__attribute__((constructor)) static void start(void) {
    const char *path=getenv("BB_PEEK_FILE");
    if (!path) return;
    pthread_t thread;
    if (!pthread_create(&thread,NULL,watch,(void *)path)) pthread_detach(thread);
}
