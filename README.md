# Custom memory allocator

`my_malloc` / `my_free` (plus `my_calloc`, `my_realloc`) built on an explicit free list.

```
make check      # unit + stress tests, plain and under ASan/UBSan
make run-bench  # benchmark vs system malloc (./bench N for best-of-N)
```

## Design

| Piece | How |
|---|---|
| Heap source | `mmap`'d chunks of 1 MiB (larger for big requests). Chunks bigger than the default are unmapped once fully free. |
| Block format | 8-byte header + 8-byte footer (boundary tags) holding `size \| alloc`. Sizes are multiples of 16, so the low bits hold flags. |
| Alignment | Headers sit at `16k + 8`, so every payload is 16-byte aligned. Minimum block is 32 bytes. |
| Free list | Doubly linked, threaded through free payloads, LIFO insertion. |
| First-fit | Return the first block on the list that is large enough. |
| Best-fit | Scan the whole list for the smallest block that fits; stop early on an exact fit. |
| Splitting | If the leftover is ≥ 32 bytes, it becomes a new free block. |
| Coalescing | Immediate, on every free, using the footer of the previous block and the header of the next. Handles all four neighbour cases. |
| Sentinels | A prologue footer and an epilogue header per chunk, both marked allocated, so coalescing never crosses a chunk boundary. |
| `realloc` | Shrinks in place, grows in place by absorbing a free right neighbour, and only moves the data when it has to. |

`my_set_policy(MY_FIRST_FIT | MY_BEST_FIT)` switches the policy at runtime. `my_heap_check()` checks every invariant: header and footer match, no two free blocks sit next to each other, and the free list matches the heap. `my_get_stats()` reports fragmentation.

The allocator is not thread-safe.

## Results (Apple Silicon, macOS, `-O2`)

```
                           system    first-fit         best-fit
fixed 32B, FIFO free       20 ns     10 ns (0.5x)       9 ns (0.4x)
mixed sizes, LIFO free     37 ns     49 ns (1.3x)      50 ns (1.4x)
free odd, refill holes     31 ns   1873 ns (61x)     1625 ns (53x)
random churn (10k live)    21 ns     22 ns (1.0x)     105 ns (4.9x)
realloc growth             32 ns    101 ns (3.1x)     471 ns (15x)
```

Memory held at peak (utilization = live bytes / bytes mapped):

```
                           system          first-fit       best-fit
random churn               20 MiB (24%)    19 MiB (25%)    7 MiB (69%)
realloc growth            116 MiB (13%)    22 MiB (71%)   22 MiB (71%)
free odd, refill holes    124 MiB (77%)   111 MiB (86%)   99 MiB (96%)
```

What the numbers show:

- **A single free list is O(free blocks) per malloc.** When the list is short (the fixed-size and LIFO workloads), it matches or beats the system allocator. With tens of thousands of fragmented holes, each malloc walks the list, and it becomes 50–60x slower.
- **Best-fit trades speed for memory.** In the churn workload it uses about 3x less memory than first-fit, but it is about 5x slower because it scans the whole list.
- **First-fit with LIFO insertion fragments badly.** Its external fragmentation reaches 98–99%: the free memory is there, but it is split into small pieces.
- The 16 bytes of header and footer cost a lot on tiny blocks: a 32-byte request uses a 48-byte block.

## Possible next steps

- Segregated free lists (size classes) to make malloc close to O(1).
- Leave out the footer on allocated blocks by keeping a "previous block is allocated" bit, which saves 8 bytes per block.
- Address-ordered insertion for first-fit, which reduces fragmentation.
- A mutex or per-thread arenas for thread safety.





What the project is
When a C program calls malloc(100), something has to find 100 free bytes, remember they're in use, and take them back when free() is called. Normally the C library does this. In this project we wrote that "something" ourselves (my_malloc and my_free), then measured it against the system's built-in malloc.

It's a learning project. It shows how memory allocators work inside, and it shows the trade-off between speed and memory waste.

The files
File	What it does
allocator.h	The public API: my_malloc, my_free, my_calloc, my_realloc, my_set_policy, plus debugging helpers.
allocator.c	The allocator itself.
test.c	Tests that check every feature behaves correctly.
bench.c	The speed and memory comparison against the system malloc.
Makefile	Build commands.
README.md	Design notes and results.
How it works, step by step
1. Getting memory from the OS
The allocator asks the operating system for a large region, a 1 MiB chunk, using mmap. It then hands out small pieces of that chunk to the program. When a chunk is full, it asks for another one.

2. Blocks
The chunk is divided into blocks. Each block has a small label at both ends:


[ header | ...... your data (payload) ...... | footer ]
  8 bytes                                      8 bytes
  "size=128, in use"                           "size=128, in use"
The header lets us jump to the next block: current address + size.
The footer lets the next block look back and find the previous block.
Together these are called "boundary tags". They make merging neighbours fast (step 5).

3. Alignment
The CPU works best when data starts at addresses that are multiples of 16. Every pointer we return is 16-byte aligned, and block sizes are rounded up to multiples of 16. For example, my_malloc(100) becomes 100 + 16 bytes of labels = 116, rounded up to a 128-byte block.

4. The free list
Free blocks are linked together in a doubly-linked list. The next/prev pointers are stored inside the free block's own unused space, so the list costs no extra memory.


free_head → [free 64B] ⇄ [free 300B] ⇄ [free 900KB] → NULL
When you call my_malloc, we search this list. There are two strategies, and you switch between them with my_set_policy():

First-fit: take the first block that's big enough. The search stops early, so it's fast, but it can waste memory.
Best-fit: check every block and take the smallest one that fits. It wastes less memory, but the search is slower.
5. Splitting and merging
Splitting (in my_malloc): if we need 128 bytes and find a 1000-byte free block, we cut it in two. The program gets 128 bytes, and the other 872 bytes go back on the free list.


[ free 1000 ]   →   [ used 128 ][ free 872 ]
Merging (coalescing) (in my_free): when a block is freed, we check its neighbours. If they're free too, we merge them into one bigger block. Without this, memory would end up as many small pieces that can't hold a larger request.


[ free ][ just freed ][ free ]   →   [ one big free block ]
There are four cases to handle: neither neighbour free, only the left one, only the right one, or both. The tests check all four.

6. Extras
my_realloc: resizes a block. It grows the block in place when the next block is free, and copies the data only when it has to.
my_calloc: allocates memory and fills it with zeros.
my_heap_check(): walks the whole heap and checks that nothing is corrupted.
my_get_stats(): reports how much memory is in use, how much is free, and how fragmented it is.
What the benchmark tells us
bench.c runs five allocation patterns through the system malloc, our first-fit and our best-fit, and measures time per operation and memory used.

The short version:

With few free blocks, our allocator is as fast as the system one, or faster. On fixed 32-byte blocks it was about 2x faster.
With many scattered free blocks, it is much slower (50–60x), because each malloc walks a long list.
Best-fit uses much less memory than first-fit (about 3x less in one test), but it takes more time.
Real allocators avoid the slow case with separate lists for different block sizes, so they rarely have to search far. That's the natural next improvement for this project.

How to run it
In a terminal, in the project folder:


cd /Users/sujeet/Documents/projects/custom_memory_allocator

make check        # build and run all tests (normal + with memory-error checkers)
make run-bench    # build and run the benchmark
./bench 5         # benchmark, running each case 5 times and keeping the best
make clean        # delete the built files
make check should end with ALL PASSED (0 failures) twice. The benchmark prints one table per workload, comparing the three allocators.

Using it in your own code

#include "allocator.h"

int main(void) {
    my_set_policy(MY_BEST_FIT);          // or MY_FIRST_FIT (the default)
    int *arr = my_malloc(10 * sizeof(int));
    arr[0] = 42;
    my_free(arr);
}
Compile it together with the allocator: cc -O2 main.c allocator.c -o main.

One limitation: the allocator is not thread-safe, so use it from a single thread only.