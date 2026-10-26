#include "allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

/* ---- allocator under test ---- */

typedef struct {
    const char *name;
    void *(*malloc)(size_t);
    void  (*free)(void *);
    void *(*realloc)(void *, size_t);
    int custom;
    my_fit_policy policy;
} allocator;

static const allocator ALLOCATORS[] = {
    { "system malloc", malloc,    free,    realloc,    0, MY_FIRST_FIT },
    { "my first-fit",  my_malloc, my_free, my_realloc, 1, MY_FIRST_FIT },
    { "my best-fit",   my_malloc, my_free, my_realloc, 1, MY_BEST_FIT  },
};
#define N_ALLOCATORS (sizeof ALLOCATORS / sizeof ALLOCATORS[0])

/* ---- helpers ---- */

static uint64_t rng_state;
static uint64_t rng(void) {            /* xorshift64 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Mostly small sizes with a long tail, roughly like real programs. */
static size_t random_size(void) {
    uint64_t r = rng() % 100;
    if (r < 60) return 8 + rng() % 120;      /* 8..127      */
    if (r < 90) return 128 + rng() % 896;    /* 128..1023   */
    if (r < 99) return 1024 + rng() % 7168;  /* 1K..8K      */
    return 8192 + rng() % 57344;             /* 8K..64K     */
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Fragmentation snapshot taken by a workload while its live set is at
 * its largest. heap == 0 means "not available". */
typedef struct {
    size_t live;       /* bytes the program asked for and still holds */
    size_t heap;       /* bytes the allocator holds from the OS       */
    double ext_frag;   /* 1 - largest_free / total_free (custom only) */
} frag;

static void snapshot(const allocator *a, size_t live, frag *f) {
    f->live = live;
    f->ext_frag = -1;
    if (a->custom) {
        my_heap_stats s;
        my_get_stats(&s);
        f->heap = s.heap_bytes;
        if (s.free_bytes) f->ext_frag = 1.0 - (double)s.largest_free / s.free_bytes;
    } else {
#ifdef __APPLE__
        malloc_statistics_t ms;
        malloc_zone_statistics(NULL, &ms);
        f->heap = ms.size_allocated;
#endif
    }
}

/* ---- workloads; each returns the number of allocator calls made ---- */

enum { N = 100000 };
static void  *ptrs[N];
static size_t lens[N];

/* Allocate N same-sized small blocks, then free them in allocation order. */
static long wl_fixed_fifo(const allocator *a, frag *f) {
    for (int i = 0; i < N; i++) {
        ptrs[i] = a->malloc(32);
        *(char *)ptrs[i] = 1;
    }
    snapshot(a, (size_t)N * 32, f);
    for (int i = 0; i < N; i++) a->free(ptrs[i]);
    return 2L * N;
}

/* Allocate N random-sized blocks, free them in reverse order. */
static long wl_mixed_lifo(const allocator *a, frag *f) {
    size_t live = 0;
    for (int i = 0; i < N; i++) {
        size_t n = random_size();
        live += n;
        ptrs[i] = a->malloc(n);
        *(char *)ptrs[i] = 1;
    }
    snapshot(a, live, f);
    for (int i = N - 1; i >= 0; i--) a->free(ptrs[i]);
    return 2L * N;
}

/* Allocate N random-sized blocks, free every other one, then fill the
 * holes with new random-sized blocks. Classic fragmentation pattern. */
static long wl_holes(const allocator *a, frag *f) {
    size_t live = 0;
    for (int i = 0; i < N; i++) {
        lens[i] = random_size();
        live += lens[i];
        ptrs[i] = a->malloc(lens[i]);
    }
    for (int i = 0; i < N; i += 2) {
        a->free(ptrs[i]);
        live -= lens[i];
    }
    for (int i = 0; i < N; i += 2) {
        lens[i] = random_size();
        live += lens[i];
        ptrs[i] = a->malloc(lens[i]);
        *(char *)ptrs[i] = 1;
    }
    snapshot(a, live, f);
    for (int i = 0; i < N; i++) a->free(ptrs[i]);
    return 3L * N;
}

/* Random churn on a live set of SLOTS: each op frees an occupied slot or
 * fills an empty one. The free list stays long, which is the worst case
 * for a single list. */
static long wl_churn(const allocator *a, frag *f) {
    enum { SLOTS = 10000, OPS = 400000 };
    size_t live = 0;
    memset(ptrs, 0, SLOTS * sizeof ptrs[0]);
    for (int op = 0; op < OPS; op++) {
        int i = (int)(rng() % SLOTS);
        if (ptrs[i]) {
            a->free(ptrs[i]);
            ptrs[i] = NULL;
            live -= lens[i];
        } else {
            lens[i] = random_size();
            ptrs[i] = a->malloc(lens[i]);
            *(char *)ptrs[i] = 1;
            live += lens[i];
        }
    }
    snapshot(a, live, f);
    for (int i = 0; i < SLOTS; i++) a->free(ptrs[i]);
    return OPS + SLOTS;
}

/* Grow 1000 buffers 64 bytes at a time, interleaved, up to 16 KiB. */
static long wl_realloc(const allocator *a, frag *f) {
    enum { BUFS = 1000, STEP = 64, MAX = 16384 };
    long calls = 0;
    memset(ptrs, 0, BUFS * sizeof ptrs[0]);
    for (size_t sz = STEP; sz <= MAX; sz += STEP) {
        for (int i = 0; i < BUFS; i++) {
            ptrs[i] = a->realloc(ptrs[i], sz);
            ((char *)ptrs[i])[sz - 1] = 1;
            calls++;
        }
    }
    snapshot(a, (size_t)BUFS * MAX, f);
    for (int i = 0; i < BUFS; i++) a->free(ptrs[i]);
    return calls + BUFS;
}

typedef struct {
    const char *name;
    long (*run)(const allocator *, frag *);
} workload;

static const workload WORKLOADS[] = {
    { "fixed 32B, FIFO free", wl_fixed_fifo },
    { "mixed sizes, LIFO free", wl_mixed_lifo },
    { "free odd, refill holes", wl_holes },
    { "random churn (10k live)", wl_churn },
    { "realloc growth", wl_realloc },
};

/* ---- driver ---- */

static void fmt_bytes(char *buf, size_t n, size_t bytes) {
    if (bytes == 0) snprintf(buf, n, "-");
    else snprintf(buf, n, "%.1f MiB", bytes / (1024.0 * 1024.0));
}

typedef struct {
    double ns;
    frag   f;
    int    ok;
} cell;

static cell run_cell(const allocator *a, const workload *w, int reps) {
    cell c = { .ok = 1 };
    double best = 1e30;
    long calls = 0;

    for (int r = 0; r < reps; r++) {
        if (a->custom) {
            my_heap_reset();
            my_set_policy(a->policy);
        }
        rng_state = 0x9E3779B97F4A7C15ull;   /* same sequence for everyone */
        double t0 = now();
        calls = w->run(a, &c.f);
        double t = now() - t0;
        if (t < best) best = t;
    }
    if (a->custom && my_heap_check() != 0) c.ok = 0;
    c.ns = best * 1e9 / calls;
    return c;
}

/* Run each cell in a fresh child process so no allocator inherits memory
 * (or a warmed-up heap) left behind by an earlier workload. */
static cell run_isolated(const allocator *a, const workload *w, int reps) {
    cell c = { .ok = 0 };
    int fd[2];
    if (pipe(fd) < 0) return c;

    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        cell r = run_cell(a, w, reps);
        ssize_t n = write(fd[1], &r, sizeof r);
        _exit(n == (ssize_t)sizeof r ? 0 : 1);
    }
    close(fd[1]);
    if (pid > 0 && read(fd[0], &c, sizeof c) != (ssize_t)sizeof c) c.ok = 0;
    close(fd[0]);
    if (pid > 0) waitpid(pid, NULL, 0);
    return c;
}

int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 3;
    if (reps < 1) reps = 1;

    printf("Best of %d run%s per cell, each cell in a fresh process.\n",
           reps, reps == 1 ? "" : "s");
    printf("ns/op = wall time / allocator calls.\n");
    printf("heap = memory held from the OS at peak; util = live / heap;\n");
    printf("ext.frag = 1 - largest free block / total free (custom allocator only).\n");

    for (size_t w = 0; w < sizeof WORKLOADS / sizeof WORKLOADS[0]; w++) {
        printf("\n== %s ==\n", WORKLOADS[w].name);
        printf("  %-14s %10s %9s %11s %7s %9s\n",
               "allocator", "ns/op", "vs sys", "heap", "util", "ext.frag");

        double sys_ns = 0;
        for (size_t k = 0; k < N_ALLOCATORS; k++) {
            const allocator *a = &ALLOCATORS[k];
            cell c = run_isolated(a, &WORKLOADS[w], reps);
            if (!c.ok) {
                fprintf(stderr, "%s / %s failed\n", a->name, WORKLOADS[w].name);
                return 1;
            }
            if (k == 0) sys_ns = c.ns;

            char heap[32], util[16], ext[16];
            fmt_bytes(heap, sizeof heap, c.f.heap);
            if (c.f.heap) snprintf(util, sizeof util, "%.0f%%", 100.0 * c.f.live / c.f.heap);
            else snprintf(util, sizeof util, "-");
            if (c.f.ext_frag >= 0) snprintf(ext, sizeof ext, "%.0f%%", 100.0 * c.f.ext_frag);
            else snprintf(ext, sizeof ext, "-");

            printf("  %-14s %10.1f %8.2fx %11s %7s %9s\n",
                   a->name, c.ns, c.ns / sys_ns, heap, util, ext);
            fflush(stdout);
        }
    }
    return 0;
}
