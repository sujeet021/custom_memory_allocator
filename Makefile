CC      ?= cc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic

all: test bench

test: test.c allocator.c allocator.h
	$(CC) $(CFLAGS) -o $@ test.c allocator.c

bench: bench.c allocator.c allocator.h
	$(CC) $(CFLAGS) -o $@ bench.c allocator.c

# Tests under AddressSanitizer + UndefinedBehaviorSanitizer.
test-san: test.c allocator.c allocator.h
	$(CC) -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $@ test.c allocator.c

check: test test-san
	./test
	./test-san

run-bench: bench
	./bench

clean:
	rm -rf test test-san bench *.dSYM

.PHONY: all check run-bench clean
