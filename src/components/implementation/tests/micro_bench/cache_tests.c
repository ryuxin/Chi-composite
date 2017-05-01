#include "micro_booter.h"
#include <stdlib.h>

#define NO_PRINT_TITLES
#define CACHE_LINE 64
#define PAGE_SIZE  4096
#define MEM_SZ     (1<<26) 	/* 64MB */
#define ITER       256
#define MEM_ITEMS  (MEM_SZ/CACHE_LINE)

struct cache_line {
	unsigned int v;
	struct cache_line *next; /* random access */
} __attribute__((aligned(CACHE_LINE)));

struct cache_line mem[MEM_ITEMS];
int rand_mem[MEM_ITEMS];

typedef enum {
	SIZES,
	READ,
	FR, 			/* flush, read */
	FRF, 			/* flush, read, flush */
	WRITE,
	FLUSH,
	FLUSHOPT,
} access_t;

typedef enum {
	SEQUENTIAL,
	RANDOM
} pattern_t;

inline void
clflush(volatile void *p)
{ asm volatile ("clflush (%0)" :: "r"(p) : "memory"); }

inline void
update(volatile void *p)
{ asm volatile ("clflush (%0) ; lfence" :: "r"(p) : "memory"); }

inline void
clflushopt(volatile void *p)
{ asm volatile ("clflushopt (%0)" :: "r"(p) : "memory"); }

void
init_random_access(void)
{
	int i, t, j;

	for (i = 0 ; i < MEM_ITEMS ; i++) {
		rand_mem[i] = (i+1) % MEM_ITEMS;
	}
	for (i = 0 ; i < MEM_ITEMS-1 ; i++) {
		j             = rand() % (MEM_ITEMS-i);
		t             = rand_mem[i];
		rand_mem[i]   = rand_mem[i+j];
		rand_mem[i+j] = t;
	}
	for (i = 0 ; i < MEM_ITEMS ; i++) {
		mem[i].next = &mem[rand_mem[i]];
	}
}

/* in global scope to force the compiler to emit the writes */
unsigned int accum = 0;

static inline void
walk(access_t how, pattern_t pat, size_t sz)
{
	unsigned int i;
	struct cache_line *line = &mem[0];

	assert(sz >= CACHE_LINE);
	for (i = 0 ; i < sz/CACHE_LINE ; i++) {

		switch(pat) {
		case SEQUENTIAL: {
			line = &mem[i];
			break;
		}
		case RANDOM: {
			struct cache_line *next = line->next;

			if (how == FRF) clflushopt(line);
			line = next;
			break;
		}
		}

		switch(how) {
		case READ:     accum    = line->v; break;
		case FR:
		case FRF:      update(line); accum = line->v; break;
		case WRITE:    line->v += i;       break;
		case FLUSH:    clflush(line);      break;
		case FLUSHOPT: clflushopt(line);   break;
		case SIZES:    return;
		}
	}
	if (how == FLUSHOPT || how == FLUSH) asm volatile ("mfence"); /* serialize */
}

/*
 * Perform a number of operations, the last one timed, for a number of different memory sizes.
 */
static inline void
exec(char *name, access_t *how_ops, size_t nops, pattern_t pat)
{
	size_t i;
	int iter;
	uint64_t start, end, tot = 0, overhead;
	static unsigned long sizes[] = {1<<6, 1<<8, 1<<10, 1<<12, 1<<14, 1<<16, 1<<18, 1<<20, 1<<22, 1<<24, 1<<26};
	(void)name;

	for (iter = 0 ; iter < ITER ; iter++) {
		rdtscll(start);
		rdtscll(end);
		assert(end-start > 0);
		tot  += end-start;
	}
	overhead = tot;
	tot      = 0;

	assert(how_ops && nops > 0);
#ifndef NO_PRINT_TITLES
	printc("%20s\t", name);
#endif
	for (i = 0 ; i < sizeof(sizes)/sizeof(unsigned long) ; i++) {
		for (iter = 0 ; iter < ITER ; iter++) {
			unsigned int j;

			for (j = 0 ; j < nops-1 ; j++) {
				walk(how_ops[j], pat, sizes[i]);
			}
			rdtscll(start);
			walk(how_ops[nops-1], pat, sizes[i]);
			rdtscll(end);
			tot  += end-start;
		}
		if (how_ops[nops-1] != SIZES) printc("%8llu\t", (tot-overhead)/((sizes[i]/CACHE_LINE)*ITER));
		else                          printc("%8lu\t", sizes[i]);
	}
	printc("\n");
}

int
cache_op_test(void)
{
//	set_prio();
	init_random_access();

//	printf("Cycles per cache-line of the operations last in the list of operations (sequential)\n\n");

	exec("Sizes", (access_t[1]){SIZES}, 1, SEQUENTIAL);

//	exec("Warmup", (access_t[3]){READ, READ, READ}, 3, SEQUENTIAL);
//	exec("Read", (access_t[2]){READ, READ}, 2, SEQUENTIAL);
	exec("Read", (access_t[2]){READ, READ}, 2, RANDOM);
//	exec("Flush/Read/Flush", (access_t[2]){READ, FRF}, 2, RANDOM);
	exec("Flush/Read", (access_t[2]){READ, FR}, 2, RANDOM);
	//exec("Modify", (access_t[2]){READ, WRITE}, 2, SEQUENTIAL);

//	exec("Flush + read", (access_t[2]){FLUSHOPT, READ}, 2, SEQUENTIAL);
//	exec("Flush + modify", (access_t[2]){FLUSHOPT, WRITE}, 2, SEQUENTIAL);

	exec("Read + flush", (access_t[2]){READ, FLUSH}, 2, SEQUENTIAL);
	exec("Modify + flush", (access_t[2]){WRITE, FLUSH}, 2, SEQUENTIAL);
	exec("Flush + flush", (access_t[2]){FLUSH, FLUSH}, 2, SEQUENTIAL);

	exec("Read + flushopt", (access_t[2]){READ, FLUSHOPT}, 2, SEQUENTIAL);
	exec("Modify + flushopt", (access_t[2]){WRITE, FLUSHOPT}, 2, SEQUENTIAL);
	exec("Flush + flushopt", (access_t[2]){FLUSH, FLUSHOPT}, 2, SEQUENTIAL);

	/* printf("Cycles per page of the operations last in the list of operations (random)\n\n"); */

	/* exec("Sizes", (access_t[1]){SIZES}, 1, RANDOM); */

	/* exec("Fault in memory", (access_t[1]){READ}, 1, RANDOM); */

	/* exec("Read", (access_t[1]){READ}, 1, RANDOM); */
	/* exec("Flush + read", (access_t[2]){FLUSHOPT, READ}, 2, RANDOM); */

	/* exec("Modify", (access_t[1]){WRITE}, 1, RANDOM); */
	/* exec("Flush + modify", (access_t[2]){FLUSHOPT, WRITE}, 2, RANDOM); */

	/* exec("Read + flush", (access_t[2]){READ, FLUSH}, 2, RANDOM); */
	/* exec("Modify + flush", (access_t[2]){WRITE, FLUSH}, 2, RANDOM); */
	/* exec("Flush + flush", (access_t[2]){FLUSH, FLUSH}, 2, RANDOM); */

	/* exec("Read + flushopt", (access_t[2]){READ, FLUSHOPT}, 2, RANDOM); */
	/* exec("Modify + flushopt", (access_t[2]){WRITE, FLUSHOPT}, 2, RANDOM); */
	/* exec("Flush + flushopt", (access_t[2]){FLUSH, FLUSHOPT}, 2, RANDOM); */

	return 0;
}
