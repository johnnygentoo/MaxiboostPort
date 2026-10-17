/* Kernel event flags: a 64-bit pattern that threads wait on (all or any of the bits), set and
 * cleared by others. The handle the game keeps is opaque (a pointer on the PS4): it holds an ID.
 * Waiters are woken in arrival order; priority-ordered flags are approximated by FIFO order, as
 * the semaphores are (no PS4 priority scheduler). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
enum { ATTR_SINGLE=0x10, ATTR_MULTI=0x20, WAIT_AND=1, WAIT_OR=2, CLEAR_ALL=0x10, CLEAR_PAT=0x20 };
typedef struct Flag {
    uint64_t id, pattern;
    uint32_t attr;
    unsigned waiting, active;
    int deleted;
    uint64_t generation; /* bumped by every set, cancel and delete: waiters re-check */
    pthread_cond_t changed;
    struct Flag *next;
    char name[32];
} Flag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Flag *registry;
static uint64_t next_id=1;
static size_t created, waits, timed_out;
static Flag *find(uint64_t id) {
    for (Flag *f=registry;f;f=f->next) if (f->id==id && !f->deleted) return f;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); exit(21); }
}
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static int matches(uint64_t pattern,uint64_t bits,uint32_t mode) {
    return (mode&3)==WAIT_AND ? (pattern&bits)==bits : (pattern&bits)!=0;
}
static ABI int32_t flag_create(uint64_t *out,const char *name,uint32_t attr,uint64_t initial,const void *options) {
    if (!out || !name || options) return ERR(22);
    const uint32_t queue=attr&0xf, threads=attr&0xf0;
    if (queue>2 || (threads && threads!=ATTR_SINGLE && threads!=ATTR_MULTI) || (attr&~0xffu)) return ERR(22);
    Flag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_condattr_t ca;
    host_check(pthread_condattr_init(&ca));
    host_check(pthread_condattr_setclock(&ca,CLOCK_MONOTONIC));
    host_check(pthread_cond_init(&f->changed,&ca));
    host_check(pthread_condattr_destroy(&ca));
    snprintf(f->name,sizeof(f->name),"%s",name);
    f->attr=threads ? attr : attr|ATTR_SINGLE; f->pattern=initial;
    pthread_mutex_lock(&lock);
    f->id=next_id++; f->next=registry; registry=f; *out=f->id; ++created;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_delete(uint64_t id) {
    pthread_mutex_lock(&lock);
    Flag **p=&registry;
    while (*p && ((*p)->id!=id || (*p)->deleted)) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Flag *f=*p;
    f->deleted=1; ++f->generation;
    host_check(pthread_cond_broadcast(&f->changed));
    if (!f->active) { *p=f->next; pthread_cond_destroy(&f->changed); free(f); }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_set(uint64_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->pattern|=bits; ++f->generation;
    host_check(pthread_cond_broadcast(&f->changed));
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_clear(uint64_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->pattern&=bits; /* the bits to keep, as on the PS4 */
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_cancel(uint64_t id,uint64_t pattern,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    if (waiters) *waiters=(int32_t)f->waiting;
    f->pattern=pattern; f->generation|=UINT64_C(1)<<63; ++f->generation; /* high bit: cancelled */
    host_check(pthread_cond_broadcast(&f->changed));
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t wait_flag(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    const uint32_t kind=mode&3, clear=mode&0x30;
    if (!bits || (kind!=WAIT_AND && kind!=WAIT_OR) || clear==(CLEAR_ALL|CLEAR_PAT) || (mode&~0x33u)) return ERR(22);
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    int32_t status=0;
    if (!matches(f->pattern,bits,mode)) {
        if (!block) status=ERR(16);
        else if ((f->attr&ATTR_SINGLE) && f->waiting) status=ERR(1);
        else if (timeout && !*timeout) { status=ERR(60); ++timed_out; }
        else {
            ++f->waiting; ++f->active; ++waits;
            const uint64_t start=runtime_wait_clock();
            const uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
            const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
            const uint64_t cancelled_before=f->generation>>63;
            for (;;) {
                if (f->deleted) { status=ERR(13); break; }
                if ((f->generation>>63)!=cancelled_before) { status=ERR(85); break; }
                if (matches(f->pattern,bits,mode)) break;
                int e=timeout ? pthread_cond_timedwait(&f->changed,&lock,&end) : pthread_cond_wait(&f->changed,&lock);
                if (e==ETIMEDOUT) {
                    if (!f->deleted && matches(f->pattern,bits,mode)) break;
                    status=ERR(60); ++timed_out; break;
                }
                host_check(e);
            }
            runtime_wait_note(2,runtime_wait_clock()-start);
            --f->waiting;
            if (timeout) {
                const uint64_t now=now_ns();
                *timeout=status ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
            }
            if (!--f->active && f->deleted) {
                Flag **p=&registry;
                while (*p!=f) p=&(*p)->next;
                *p=f->next; pthread_cond_destroy(&f->changed);
                if (result) *result=0;
                free(f);
                pthread_mutex_unlock(&lock);
                return status;
            }
        }
    }
    if (result) *result=f->pattern;
    if (!status && clear==CLEAR_ALL) f->pattern=0;
    else if (!status && clear==CLEAR_PAT) f->pattern&=~bits;
    pthread_mutex_unlock(&lock);
    return status;
}
static ABI int32_t flag_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return wait_flag(id,bits,mode,result,timeout,1);
}
static ABI int32_t flag_poll(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result) {
    return wait_flag(id,bits,mode,result,NULL,0);
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",flag_create}, {"sceKernelDeleteEventFlag",flag_delete},
    {"sceKernelSetEventFlag",flag_set}, {"sceKernelClearEventFlag",flag_clear},
    {"sceKernelCancelEventFlag",flag_cancel}, {"sceKernelWaitEventFlag",flag_wait},
    {"sceKernelPollEventFlag",flag_poll},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, blocking waits=%zu, timeouts=%zu\n",created,waits,timed_out);
    pthread_mutex_unlock(&lock);
}
#else
uintptr_t runtime_eventflag_resolve(const char *name) { (void)name; return 0; }
void runtime_eventflag_report(void) {}
#endif
