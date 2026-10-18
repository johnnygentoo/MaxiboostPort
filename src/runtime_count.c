/* BB_GUEST_COUNT=<offset>[,<offset>...]: how often the game's code reaches these instructions
 * (image offsets, as guest+0x... in the traces), reported every two seconds:
 *   Guest counts (2 s): +0x5a6223=118 +0x5a62ac=0
 * Each site gets an int3; the trap counts it, puts the original byte back, single-steps that one
 * instruction and arms the int3 again. A diagnostic: a thread that runs the site while another
 * steps over it is not counted, and every hit costs two signals. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_SITES 256
#define TRAP_FLAG 0x100ull

#define MAX_CALLERS 8
static struct {
    uint64_t offset; unsigned char *code; unsigned char original; _Atomic uint64_t hits;
    _Atomic uint64_t callers[MAX_CALLERS]; /* return addresses seen at the site (function entries) */
    _Atomic uint64_t rdis[MAX_CALLERS];    /* first distinct rdi values there (the object, at entries) */
} sites[MAX_SITES];
static unsigned char *image_base;
/* BB_GUEST_DELAY=<offset>:<microseconds>: the thread reaching that site sleeps first (ordering
 * experiments: does the game read GPU results too early?). The site must also be counted. */
static uint64_t delay_offset, delay_us;
/* BB_GUEST_REG=<offset>:<reg>: the distinct values a register holds at that site (reg: a REG_*
 * index name such as rdx, edx counts the low 32 bits). BB_GUEST_LIST=<offset>:<reg>: at that site
 * the register points at the effect draw list (32-byte entries: id <= 0xfff, count != 0); the
 * number of leading valid entries is counted and the first entries of the last hit are shown. */
static uint64_t reg_offset, list_offset; static int reg_index = -1, list_index = -1; static int reg_low;
static _Atomic uint64_t reg_values[16], reg_counts[16];
static _Atomic uint64_t list_hist[6]; static uint32_t list_last[24];
static int reg_by_name(const char *name, int *low) {
    static const struct { const char *n; int r; } names[] = {{"rax", REG_RAX}, {"rbx", REG_RBX}, {"rcx", REG_RCX},
        {"rdx", REG_RDX}, {"rsi", REG_RSI}, {"rdi", REG_RDI}, {"r8", REG_R8}, {"r9", REG_R9},
        {"r12", REG_R12}, {"r13", REG_R13}, {"r14", REG_R14}, {"r15", REG_R15}};
    *low = name[0] == 'e';
    char wide[8]; snprintf(wide, sizeof(wide), "%s%s", *low ? "r" : "", *low ? name + 1 : name);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(names[i].n, wide)) return names[i].r;
    return -1;
}
static int site_count;
static struct sigaction previous;
/* The site each thread is stepping over, by thread id: no thread-local storage in the handler
 * (the game's threads run with the guest's TLS register setup). */
#define MAX_STEPPERS 256
static struct { _Atomic int tid; int site; } steppers[MAX_STEPPERS];
static int *stepper_slot(int tid, int create) {
    for (int i = 0; i < MAX_STEPPERS; ++i) {
        const int k = (int)(((unsigned)tid * 2654435761u + (unsigned)i) % MAX_STEPPERS);
        int seen = atomic_load(&steppers[k].tid);
        if (seen == tid) return &steppers[k].site;
        if (seen == 0) {
            if (!create) return NULL;
            int expected = 0;
            if (atomic_compare_exchange_strong(&steppers[k].tid, &expected, tid)) return &steppers[k].site;
            if (expected == tid) return &steppers[k].site;
        }
    }
    return NULL;
}

/* The sites' pages stay writable (made so once at install): a hit is two signals, no mprotect. */
static void write_byte(unsigned char *code, unsigned char value) {
    *(volatile unsigned char *)code = value;
}

static void on_trap_body(int sig, siginfo_t *info, void *context);
static void on_trap(int sig, siginfo_t *info, void *context) {
    const int saved_errno = errno;
    on_trap_body(sig, info, context);
    errno = saved_errno;
}
static void on_trap_body(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = (ucontext_t *)context;
    uint64_t *rip = (uint64_t *)&uc->uc_mcontext.gregs[REG_RIP];
    uint64_t *flags = (uint64_t *)&uc->uc_mcontext.gregs[REG_EFL];
    const int tid = (int)syscall(SYS_gettid);
    int *stepping = stepper_slot(tid, 0);
    /* The kernel tells a single step (TRAP_TRACE) from an int3 (SI_KERNEL): a thread can meet a
     * site's int3 re-armed by another thread while it steps over it. */
    if (info->si_code == TRAP_TRACE && stepping && *stepping >= 0) { /* the instruction ran */
        write_byte(sites[*stepping].code, 0xcc);
        *stepping = -1;
        *flags &= ~TRAP_FLAG;
        return;
    }
    for (int i = 0; i < site_count; ++i) {
        if ((uint64_t)(uintptr_t)sites[i].code == *rip - 1) {
            atomic_fetch_add_explicit(&sites[i].hits, 1, memory_order_relaxed);
            if (reg_index >= 0 && sites[i].offset == reg_offset) {
                uint64_t v = (uint64_t)uc->uc_mcontext.gregs[reg_index];
                if (reg_low) v &= 0xffffffffull;
                for (int k = 0; k < 16; ++k) {
                    uint64_t seen = atomic_load(&reg_values[k]);
                    if (seen == v + 1) { atomic_fetch_add(&reg_counts[k], 1); break; }
                    if (seen == 0 && atomic_compare_exchange_strong(&reg_values[k], &seen, v + 1)) {
                        atomic_fetch_add(&reg_counts[k], 1); break;
                    }
                }
            }
            if (list_index >= 0 && sites[i].offset == list_offset) {
                const uint32_t *e = (const uint32_t *)(uintptr_t)uc->uc_mcontext.gregs[list_index];
                int n = 0;
                while (n < 0x4000 && e[n * 8] <= 0xfff && e[n * 8 + 1] != 0) ++n;
                atomic_fetch_add(&list_hist[n == 0 ? 0 : n == 1 ? 1 : n <= 4 ? 2 : n <= 16 ? 3 : n <= 64 ? 4 : 5], 1);
                memcpy(list_last, e, sizeof(list_last));
            }
            if (delay_us && sites[i].offset == delay_offset) {
                struct timespec t = {(time_t)(delay_us / 1000000), (long)(delay_us % 1000000) * 1000};
                nanosleep(&t, NULL);
            }
            const uint64_t ret = *(const uint64_t *)(uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
            for (int k = 0; k < MAX_CALLERS; ++k) {
                uint64_t seen = atomic_load(&sites[i].callers[k]);
                if (seen == ret) break;
                if (seen == 0 && atomic_compare_exchange_strong(&sites[i].callers[k], &seen, ret)) break;
            }
            const uint64_t rdi = (uint64_t)uc->uc_mcontext.gregs[REG_RDI];
            for (int k = 0; k < MAX_CALLERS; ++k) {
                uint64_t seen = atomic_load(&sites[i].rdis[k]);
                if (seen == rdi) break;
                if (seen == 0 && atomic_compare_exchange_strong(&sites[i].rdis[k], &seen, rdi)) break;
            }
            write_byte(sites[i].code, sites[i].original);
            *rip -= 1;
            *flags |= TRAP_FLAG;
            if (!stepping) stepping = stepper_slot(tid, 1);
            if (stepping) *stepping = i;
            return;
        }
    }
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) previous.sa_sigaction(sig, info, context);
    } else if (previous.sa_handler != SIG_IGN && previous.sa_handler != SIG_DFL && previous.sa_handler) {
        previous.sa_handler(sig);
    } else {
        signal(SIGTRAP, SIG_DFL);
        raise(SIGTRAP);
    }
}

static void *report(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec t = {2, 0};
        nanosleep(&t, NULL);
        printf("Guest counts (2 s):");
        for (int i = 0; i < site_count; ++i)
            printf(" +%#" PRIx64 "=%" PRIu64, sites[i].offset, atomic_exchange(&sites[i].hits, 0));
        printf("\n");
        /* Callers (return addresses in the image), for sites at function entries. */
        printf("Guest callers:");
        for (int i = 0; i < site_count; ++i) {
            printf(" +%#" PRIx64 "<", sites[i].offset);
            for (int k = 0; k < MAX_CALLERS; ++k) {
                const uint64_t ret = atomic_load(&sites[i].callers[k]);
                if (!ret) break;
                const uint64_t base = (uint64_t)(uintptr_t)image_base;
                if (ret >= base && ret < base + 0x5000000) printf("%s+%#" PRIx64, k ? "," : "", ret - base);
                else printf("%s?", k ? "," : "");
            }
        }
        printf("\n");
        if (reg_index >= 0) {
            printf("Guest reg at +%#" PRIx64 ":", reg_offset);
            for (int k = 0; k < 16; ++k) {
                const uint64_t v = atomic_load(&reg_values[k]);
                if (!v) break;
                printf(" %#" PRIx64 "x%" PRIu64, v - 1, atomic_exchange(&reg_counts[k], 0));
            }
            printf("\n");
        }
        if (list_index >= 0) {
            printf("Guest list at +%#" PRIx64 ": valid 0:%" PRIu64 " 1:%" PRIu64 " 2-4:%" PRIu64
                   " 5-16:%" PRIu64 " 17-64:%" PRIu64 " 65+:%" PRIu64 "; last", list_offset,
                   atomic_exchange(&list_hist[0], 0), atomic_exchange(&list_hist[1], 0),
                   atomic_exchange(&list_hist[2], 0), atomic_exchange(&list_hist[3], 0),
                   atomic_exchange(&list_hist[4], 0), atomic_exchange(&list_hist[5], 0));
            for (int k = 0; k < 24; ++k) printf("%s%08x", k % 8 ? " " : " | ", list_last[k]);
            printf("\n");
        }
        printf("Guest rdi:");
        for (int i = 0; i < site_count; ++i) {
            printf(" +%#" PRIx64 "<", sites[i].offset);
            for (int k = 0; k < MAX_CALLERS; ++k) {
                const uint64_t v = atomic_load(&sites[i].rdis[k]);
                if (!v) break;
                printf("%s%#" PRIx64, k ? "," : "", v);
            }
        }
        printf("\n");
        fflush(stdout);
    }
    return NULL;
}

void runtime_count_install(unsigned char *image) {
    const char *list = getenv("BB_GUEST_COUNT");
    if (!list || !*list) return;
    image_base = image;
    for (int which = 0; which < 2; ++which) {
        const char *spec = getenv(which ? "BB_GUEST_LIST" : "BB_GUEST_REG");
        if (!spec || !*spec) continue;
        char *end = NULL;
        const uint64_t offset = strtoull(spec, &end, 16);
        int low = 0;
        const int index = end && *end == ':' ? reg_by_name(end + 1, &low) : -1;
        if (which) { list_offset = offset; list_index = index; }
        else { reg_offset = offset; reg_index = index; reg_low = low; }
    }
    const char *delay = getenv("BB_GUEST_DELAY");
    if (delay && *delay) {
        char *end = NULL;
        delay_offset = strtoull(delay, &end, 16);
        delay_us = end && *end == ':' ? strtoull(end + 1, NULL, 10) : 0;
    }
    for (const char *at = list; at && *at && site_count < MAX_SITES;) {
        char *end = NULL;
        const uint64_t offset = strtoull(at, &end, 16);
        sites[site_count].offset = offset;
        sites[site_count].code = image + offset;
        sites[site_count].original = image[offset];
        ++site_count;
        at = end && *end == ',' ? end + 1 : NULL;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_trap;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous);
    for (int i = 0; i < site_count; ++i) {
        const uintptr_t page = (uintptr_t)sites[i].code & ~(uintptr_t)4095;
        mprotect((void *)page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
        write_byte(sites[i].code, 0xcc);
    }
    pthread_t thread;
    if (!pthread_create(&thread, NULL, report, NULL)) pthread_detach(thread);
    printf("Guest counts: %d sites (BB_GUEST_COUNT)\n", site_count);
}
