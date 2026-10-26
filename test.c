#include "allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define EXPECT(cond)                                                   \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

#define RUN(fn)                                       \
    do {                                              \
        int before = failures;                        \
        my_heap_reset();                              \
        my_set_policy(MY_FIRST_FIT);                  \
        fn();                                         \
        EXPECT(my_heap_check() == 0);                 \
        printf("%-40s %s\n", #fn, failures == before ? "ok" : "FAILED"); \
    } while (0)

static my_heap_stats stats(void) {
    my_heap_stats s;
    my_get_stats(&s);
    return s;
}

static void test_alignment(void) {
    for (size_t n = 1; n <= 300; n++) {
        void *p = my_malloc(n);
        EXPECT(p != NULL);
        EXPECT((uintptr_t)p % 16 == 0);
        EXPECT(my_usable_size(p) >= n);
    }
    EXPECT(my_malloc(0) == NULL);
    my_free(NULL);
}

static void test_split(void) {
    void *p = my_malloc(100);              /* 100 + 16 -> 128-byte block */
    my_heap_stats s = stats();
    EXPECT(s.alloc_blocks == 1);
    EXPECT(s.alloc_bytes == 128);
    EXPECT(s.free_blocks == 1);            /* remainder of the chunk */
    EXPECT(s.free_bytes + s.alloc_bytes + 32 == s.heap_bytes);

    my_free(p);
    s = stats();
    EXPECT(s.alloc_blocks == 0);
    EXPECT(s.free_blocks == 1);            /* merged back into one block */
}

static void test_no_split_when_remainder_too_small(void) {
    void *a = my_malloc(48);               /* 64-byte block */
    void *guard = my_malloc(16);
    my_free(a);
    void *b = my_malloc(32);               /* needs 48, remainder 16 < 32 */
    EXPECT(b == a);
    EXPECT(my_usable_size(b) == 48);       /* got the whole block */
    my_free(b);
    my_free(guard);
}

static void test_coalesce_all_cases(void) {
    /* Five adjacent 64-byte blocks, then the rest of the chunk. */
    void *a = my_malloc(48), *b = my_malloc(48), *c = my_malloc(48),
         *d = my_malloc(48), *e = my_malloc(48);

    my_free(a);                            /* no free neighbours */
    my_free(c);                            /* no free neighbours */
    EXPECT(stats().free_blocks == 3);
    my_free(b);                            /* both neighbours free */
    EXPECT(stats().free_blocks == 2);
    EXPECT(stats().largest_free > 0);

    /* a+b+c is now one 192-byte block that fits exactly 176 bytes. */
    void *abc = my_malloc(176);
    EXPECT(abc == a);
    my_free(abc);

    my_free(e);                            /* merges with right neighbour */
    EXPECT(stats().free_blocks == 2);
    my_free(d);                            /* merges left and right */
    my_heap_stats s = stats();
    EXPECT(s.free_blocks == 1);
    EXPECT(s.alloc_blocks == 0);
}

static void test_first_vs_best_fit(void) {
    void *big = my_malloc(256);            /* 272-byte block */
    void *g1 = my_malloc(16);
    void *small = my_malloc(64);           /* 80-byte block */
    void *g2 = my_malloc(16);

    /* LIFO list after this: big, small, chunk remainder */
    my_free(small);
    my_free(big);

    my_set_policy(MY_FIRST_FIT);
    void *p = my_malloc(50);               /* 80-byte block needed */
    EXPECT(p == big);                      /* first on the list */
    my_free(p);                            /* list: big, small, rest */

    my_set_policy(MY_BEST_FIT);
    p = my_malloc(50);
    EXPECT(p == small);                    /* exact fit */
    my_free(p);

    my_free(g1);
    my_free(g2);
}

static void test_large_alloc_releases_chunk(void) {
    void *small = my_malloc(10);
    EXPECT(stats().chunks == 1);
    size_t big = 8u << 20;
    char *p = my_malloc(big);
    EXPECT(p != NULL);
    memset(p, 0xAB, big);
    EXPECT(stats().chunks == 2);
    my_free(p);
    EXPECT(stats().chunks == 1);
    my_free(small);
}

static void test_calloc(void) {
    unsigned char *p = my_malloc(256);
    memset(p, 0xFF, 256);
    my_free(p);
    unsigned char *q = my_calloc(64, 4);
    int zero = 1;
    for (int i = 0; i < 256; i++) zero &= q[i] == 0;
    EXPECT(zero);
    EXPECT(my_calloc(SIZE_MAX / 2, 4) == NULL);
    my_free(q);
}

static void test_realloc(void) {
    char *p = my_realloc(NULL, 40);
    for (int i = 0; i < 40; i++) p[i] = (char)i;

    /* Grow in place: nothing after p but the free chunk remainder. */
    char *q = my_realloc(p, 400);
    EXPECT(q == p);

    /* Force a move by blocking the right-hand side. */
    void *guard = my_malloc(16);
    char *r = my_realloc(q, 4000);
    EXPECT(r != q);
    int same = 1;
    for (int i = 0; i < 40; i++) same &= r[i] == (char)i;
    EXPECT(same);

    /* Shrink in place. */
    char *s = my_realloc(r, 20);
    EXPECT(s == r);
    EXPECT(my_usable_size(s) == 32);       /* 20 + 16 -> 48-byte block */

    EXPECT(my_realloc(s, 0) == NULL);
    my_free(guard);
    EXPECT(stats().alloc_blocks == 0);
}

static void stress(my_fit_policy policy) {
    enum { SLOTS = 2000, OPS = 200000 };
    static unsigned char *ptr[SLOTS];
    static size_t len[SLOTS];
    memset(ptr, 0, sizeof ptr);
    my_set_policy(policy);
    srand(42);

    for (int op = 0; op < OPS; op++) {
        int i = rand() % SLOTS;
        if (ptr[i]) {
            int ok = 1;
            for (size_t k = 0; k < len[i]; k++) ok &= ptr[i][k] == (unsigned char)i;
            EXPECT(ok);
            if (rand() % 4 == 0) {
                len[i] = 1 + rand() % 3000;
                ptr[i] = my_realloc(ptr[i], len[i]);
                memset(ptr[i], i & 0xFF, len[i]);
            } else {
                my_free(ptr[i]);
                ptr[i] = NULL;
            }
        } else {
            len[i] = rand() % 8 == 0 ? 1 + rand() % 50000 : 1 + rand() % 512;
            ptr[i] = my_malloc(len[i]);
            EXPECT(ptr[i] != NULL);
            EXPECT((uintptr_t)ptr[i] % 16 == 0);
            memset(ptr[i], i & 0xFF, len[i]);
        }
        if (op % 5000 == 0 && my_heap_check() != 0) {
            failures++;
            return;
        }
    }
    for (int i = 0; i < SLOTS; i++) my_free(ptr[i]);
    EXPECT(stats().alloc_blocks == 0);
}

static void test_stress_first_fit(void) { stress(MY_FIRST_FIT); }
static void test_stress_best_fit(void) { stress(MY_BEST_FIT); }

int main(void) {
    RUN(test_alignment);
    RUN(test_split);
    RUN(test_no_split_when_remainder_too_small);
    RUN(test_coalesce_all_cases);
    RUN(test_first_vs_best_fit);
    RUN(test_large_alloc_releases_chunk);
    RUN(test_calloc);
    RUN(test_realloc);
    RUN(test_stress_first_fit);
    RUN(test_stress_best_fit);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
