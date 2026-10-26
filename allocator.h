#ifndef MY_ALLOCATOR_H
#define MY_ALLOCATOR_H

#include <stddef.h>

/*
 * A simple explicit-free-list allocator.
 *
 *  - Memory comes from the OS in mmap'd chunks (default 1 MiB).
 *  - Every block carries a header and a footer (boundary tags) holding
 *    its size and an "allocated" bit, so neighbours can be found in O(1).
 *  - Free blocks are kept on a doubly-linked list threaded through their
 *    payloads (LIFO insertion).
 *  - Payloads are 16-byte aligned, like the system malloc on 64-bit.
 *  - Freed blocks are immediately coalesced with free neighbours; oversized
 *    blocks are split on allocation.
 *
 * Not thread-safe.
 */

typedef enum {
    MY_FIRST_FIT,   /* take the first free block that is large enough   */
    MY_BEST_FIT     /* take the smallest free block that is large enough */
} my_fit_policy;

void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_calloc(size_t nmemb, size_t size);
void *my_realloc(void *ptr, size_t size);
size_t my_usable_size(void *ptr);

void          my_set_policy(my_fit_policy policy);
my_fit_policy my_get_policy(void);

typedef struct {
    size_t heap_bytes;    /* total bytes mapped from the OS              */
    size_t chunks;        /* number of mapped chunks                     */
    size_t alloc_blocks;  /* number of allocated blocks                  */
    size_t alloc_bytes;   /* bytes in allocated blocks (incl. overhead)  */
    size_t free_blocks;   /* number of free blocks                       */
    size_t free_bytes;    /* bytes in free blocks                        */
    size_t largest_free;  /* size of the largest free block              */
} my_heap_stats;

/* Walk the heap and fill in *out. O(number of blocks). */
void my_get_stats(my_heap_stats *out);

/* Verify heap invariants. Returns 0 if consistent, otherwise prints the
 * first problem found to stderr and returns -1. */
int my_heap_check(void);

/* Unmap every chunk. All outstanding pointers become invalid. */
void my_heap_reset(void);

#endif
