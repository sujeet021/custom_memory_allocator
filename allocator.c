#include "allocator.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Chunk layout (every offset is from the start of the mmap'd region):
 *
 *   0   chunk.next       \
 *   8   chunk.size        } CHUNK_HDR = 24 bytes
 *   16  prologue footer  /   value PROLOGUE: "size 0, allocated"
 *   24  first block header
 *   32  first payload    <- 16-byte aligned
 *   ...
 *   size-8  epilogue header  value EPILOGUE: "size 0, allocated"
 *
 * Block layout:
 *
 *   [ header: size|alloc ][ payload ... ][ footer: size|alloc ]
 *     8 bytes                              8 bytes
 *
 * Block sizes include header and footer and are multiples of 16, so the
 * low 4 bits of a size word are free for flags. Because each header sits
 * 8 bytes before a 16-byte boundary, every payload is 16-byte aligned.
 *
 * A free block stores free-list links in its payload, so the minimum
 * block is 8 (header) + 16 (two pointers) + 8 (footer) = 32 bytes.
 *
 * The prologue footer and epilogue header look like allocated neighbours,
 * so coalescing stops at chunk boundaries without special cases.
 */

#define ALIGNMENT     16
#define WSIZE         sizeof(size_t)
#define OVERHEAD      (2 * WSIZE)
#define MIN_BLOCK     32
#define CHUNK_HDR     24
#define DEFAULT_CHUNK ((size_t)1 << 20)

#define ALLOC_BIT ((size_t)1)
#define SIZE_MASK (~(size_t)(ALIGNMENT - 1))
#define PROLOGUE  ALLOC_BIT
#define EPILOGUE  ALLOC_BIT

typedef struct free_node {
    struct free_node *next;
    struct free_node *prev;
} free_node;

typedef struct chunk {
    struct chunk *next;
    size_t size;
} chunk;

static free_node    *free_head;
static chunk        *chunks;
static my_fit_policy policy = MY_FIRST_FIT;

/* ---- block helpers (b points at a block header) ---- */

static inline size_t *hdr(char *b)        { return (size_t *)b; }
static inline size_t  bsize(char *b)      { return *hdr(b) & SIZE_MASK; }
static inline int     is_alloc(char *b)   { return (int)(*hdr(b) & ALLOC_BIT); }
static inline size_t *ftr(char *b)        { return (size_t *)(b + bsize(b) - WSIZE); }
static inline char   *next_blk(char *b)   { return b + bsize(b); }
static inline size_t *prev_ftr(char *b)   { return (size_t *)(b - WSIZE); }
static inline char   *prev_blk(char *b)   { return b - (*prev_ftr(b) & SIZE_MASK); }
static inline void   *payload(char *b)    { return b + WSIZE; }
static inline char   *from_payload(void *p) { return (char *)p - WSIZE; }
static inline free_node *node(char *b)    { return (free_node *)payload(b); }

static inline void set_block(char *b, size_t size, int alloc) {
    *hdr(b) = size | (alloc ? ALLOC_BIT : 0);
    *ftr(b) = *hdr(b);
}

static inline size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

/* ---- free list ---- */

static void list_insert(char *b) {
    free_node *n = node(b);
    n->prev = NULL;
    n->next = free_head;
    if (free_head) free_head->prev = n;
    free_head = n;
}

static void list_remove(char *b) {
    free_node *n = node(b);
    if (n->prev) n->prev->next = n->next;
    else         free_head = n->next;
    if (n->next) n->next->prev = n->prev;
}

/* ---- chunks ---- */

static char *first_block(chunk *c) { return (char *)c + CHUNK_HDR; }

/* Map a new chunk big enough for a block of asize bytes and put its single
 * free block on the free list. */
static char *extend_heap(size_t asize) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = asize + CHUNK_HDR + WSIZE;
    if (size < DEFAULT_CHUNK) size = DEFAULT_CHUNK;
    size = align_up(size, page);

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    chunk *c = mem;
    c->size = size;
    c->next = chunks;
    chunks = c;

    char *b = first_block(c);
    *prev_ftr(b) = PROLOGUE;
    set_block(b, size - CHUNK_HDR - WSIZE, 0);
    *hdr(next_blk(b)) = EPILOGUE;
    list_insert(b);
    return b;
}

/* If free block b spans an entire oversized chunk, give the chunk back to
 * the OS. Default-sized chunks are kept to avoid mmap/munmap thrashing. */
static int release_chunk(char *b) {
    if (*prev_ftr(b) != PROLOGUE || *hdr(next_blk(b)) != EPILOGUE) return 0;
    chunk *c = (chunk *)(b - CHUNK_HDR);
    if (c->size <= DEFAULT_CHUNK) return 0;

    for (chunk **pp = &chunks; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            munmap(c, c->size);
            return 1;
        }
    }
    return 0;
}

/* ---- core ---- */

static char *find_fit(size_t asize) {
    if (policy == MY_FIRST_FIT) {
        for (free_node *n = free_head; n; n = n->next) {
            char *b = from_payload(n);
            if (bsize(b) >= asize) return b;
        }
        return NULL;
    }

    char  *best = NULL;
    size_t best_size = SIZE_MAX;
    for (free_node *n = free_head; n; n = n->next) {
        char  *b = from_payload(n);
        size_t s = bsize(b);
        if (s >= asize && s < best_size) {
            best = b;
            best_size = s;
            if (s == asize) break; /* can't do better than exact */
        }
    }
    return best;
}

/* Allocate asize bytes from free block b, splitting off the tail if it is
 * big enough to be a block of its own. */
static void place(char *b, size_t asize) {
    size_t total = bsize(b);
    list_remove(b);
    if (total - asize >= MIN_BLOCK) {
        set_block(b, asize, 1);
        char *rest = next_blk(b);
        set_block(rest, total - asize, 0);
        list_insert(rest);
    } else {
        set_block(b, total, 1);
    }
}

/* Merge free block b (not on the list) with free neighbours. Returns the
 * start of the merged block, which is also not on the list. */
static char *coalesce(char *b) {
    size_t size = bsize(b);
    char  *next = next_blk(b);

    if (!is_alloc(next)) {
        list_remove(next);
        size += bsize(next);
    }
    if (!(*prev_ftr(b) & ALLOC_BIT)) {
        b = prev_blk(b);
        list_remove(b);
        size += bsize(b);
    }
    set_block(b, size, 0);
    return b;
}

/* Request size -> block size: add header/footer, round to alignment. */
static int adjust_size(size_t size, size_t *asize) {
    if (size > SIZE_MAX - OVERHEAD - ALIGNMENT) return -1;
    size_t a = align_up(size + OVERHEAD, ALIGNMENT);
    *asize = a < MIN_BLOCK ? MIN_BLOCK : a;
    return 0;
}

void *my_malloc(size_t size) {
    size_t asize;
    if (size == 0) return NULL;
    if (adjust_size(size, &asize) < 0) {
        errno = ENOMEM;
        return NULL;
    }

    char *b = find_fit(asize);
    if (!b && !(b = extend_heap(asize))) {
        errno = ENOMEM;
        return NULL;
    }
    place(b, asize);
    return payload(b);
}

void my_free(void *ptr) {
    if (!ptr) return;
    char *b = from_payload(ptr);
    set_block(b, bsize(b), 0);
    b = coalesce(b);
    if (!release_chunk(b)) list_insert(b);
}

void *my_calloc(size_t nmemb, size_t size) {
    if (size && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = my_malloc(nmemb * size);
    if (p) memset(p, 0, nmemb * size);
    return p;
}

void *my_realloc(void *ptr, size_t size) {
    if (!ptr) return my_malloc(size);
    if (size == 0) {
        my_free(ptr);
        return NULL;
    }

    size_t asize;
    if (adjust_size(size, &asize) < 0) {
        errno = ENOMEM;
        return NULL;
    }

    char  *b = from_payload(ptr);
    size_t cur = bsize(b);

    /* Shrink in place, returning the tail to the free list. */
    if (asize <= cur) {
        if (cur - asize >= MIN_BLOCK) {
            set_block(b, asize, 1);
            char *rest = next_blk(b);
            set_block(rest, cur - asize, 0);
            list_insert(coalesce(rest));
        }
        return ptr;
    }

    /* Grow in place by absorbing a free right-hand neighbour. */
    char *next = next_blk(b);
    if (!is_alloc(next) && cur + bsize(next) >= asize) {
        size_t total = cur + bsize(next);
        list_remove(next);
        if (total - asize >= MIN_BLOCK) {
            set_block(b, asize, 1);
            char *rest = next_blk(b);
            set_block(rest, total - asize, 0);
            list_insert(rest);
        } else {
            set_block(b, total, 1);
        }
        return ptr;
    }

    /* Move. */
    void *np = my_malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, cur - OVERHEAD);
    my_free(ptr);
    return np;
}

size_t my_usable_size(void *ptr) {
    return ptr ? bsize(from_payload(ptr)) - OVERHEAD : 0;
}

void my_set_policy(my_fit_policy p) { policy = p; }
my_fit_policy my_get_policy(void) { return policy; }

/* ---- introspection ---- */

void my_get_stats(my_heap_stats *s) {
    memset(s, 0, sizeof *s);
    for (chunk *c = chunks; c; c = c->next) {
        s->chunks++;
        s->heap_bytes += c->size;
        for (char *b = first_block(c); *hdr(b) != EPILOGUE; b = next_blk(b)) {
            size_t sz = bsize(b);
            if (is_alloc(b)) {
                s->alloc_blocks++;
                s->alloc_bytes += sz;
            } else {
                s->free_blocks++;
                s->free_bytes += sz;
                if (sz > s->largest_free) s->largest_free = sz;
            }
        }
    }
}

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            fprintf(stderr, "heap check failed: " __VA_ARGS__); \
            fputc('\n', stderr);                          \
            return -1;                                    \
        }                                                 \
    } while (0)

int my_heap_check(void) {
    size_t free_in_heap = 0;

    for (chunk *c = chunks; c; c = c->next) {
        char *b = first_block(c);
        char *end = (char *)c + c->size - WSIZE;
        CHECK(*prev_ftr(b) == PROLOGUE, "bad prologue in chunk %p", (void *)c);
        CHECK(*hdr(end) == EPILOGUE, "bad epilogue in chunk %p", (void *)c);

        int prev_free = 0;
        for (; b != end; b = next_blk(b)) {
            size_t sz = bsize(b);
            CHECK(b < end, "block %p runs past chunk end", (void *)b);
            CHECK(sz >= MIN_BLOCK && sz % ALIGNMENT == 0, "block %p has bad size %zu", (void *)b, sz);
            CHECK((uintptr_t)payload(b) % ALIGNMENT == 0, "block %p payload misaligned", (void *)b);
            CHECK(*hdr(b) == *ftr(b), "block %p header/footer mismatch", (void *)b);
            if (!is_alloc(b)) {
                CHECK(!prev_free, "adjacent free blocks at %p (missed coalesce)", (void *)b);
                free_in_heap++;
            }
            prev_free = !is_alloc(b);
        }
    }

    size_t free_in_list = 0;
    for (free_node *n = free_head; n; n = n->next) {
        char *b = from_payload(n);
        CHECK(!is_alloc(b), "allocated block %p on free list", (void *)b);
        CHECK(n->next == NULL || n->next->prev == n, "broken list links at %p", (void *)b);
        CHECK(++free_in_list <= free_in_heap, "free list longer than heap's free blocks (cycle?)");
    }
    CHECK(free_in_list == free_in_heap, "free list has %zu blocks, heap has %zu",
          free_in_list, free_in_heap);
    return 0;
}

void my_heap_reset(void) {
    while (chunks) {
        chunk *c = chunks;
        chunks = c->next;
        munmap(c, c->size);
    }
    free_head = NULL;
}
