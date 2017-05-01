#ifndef NON_CC_H
#define NON_CC_H

#include "shared/util.h"
#include "shared/cos_types.h"
#include "assert.h"

#define NUM_CLFLUSH_ITEM 100
#define GET_QUIESCE_IDX(va) (((u32_t)(va) - ivshmem_addr) / RETYPE_MEM_SIZE) 

typedef enum {
	TLB_QUIESCENCE,
	KERNEL_QUIESCENCE,
	NON_CC_QUIESCENCE
} quiescence_type_t;

struct non_cc_quiescence {
	u64_t last_mandatory_flush;
	u8_t __padding[CACHE_LINE - sizeof(u64_t)];
} __attribute__((aligned(CACHE_LINE), packed)) ;

extern struct non_cc_quiescence *cc_quiescence;
extern u64_t *global_tsc;

int cos_quiescence_check(u64_t cur, u64_t past, u64_t grace_period, quiescence_type_t type);
int cos_cache_mandatory_flush(void);
void non_cc_init(void);

static inline void
non_cc_rdtscll(u64_t *t)
{
	if (!cur_node) {
		rdtscll(*global_tsc);
		cos_wb_cache(global_tsc);
	} else {
		cos_flush_cache(global_tsc);
	}
	*t = *global_tsc;
}

static inline int 
cos_non_cc_cas(unsigned long *target, unsigned long old, unsigned long updated)
{
	assert(VA_IN_IVSHMEM_RANGE(target));
	return cos_cas(target, old, updated);
}

static inline int 
cos_non_cc_faa(int *var, int value)
{
	assert(VA_IN_IVSHMEM_RANGE(var));
	return cos_faa(var, value);
}

static inline void
cos_clflush_range(void *s, void *e)
{
	s = (void *)round_to_cacheline(s);
	e = (void *)round_to_cacheline(e-1);
	for(; s<=e; s += CACHE_LINE) cos_flush_cache(s);
}

static inline void
cos_clwb_range(void *s, void *e)
{
	s = (void *)round_to_cacheline(s);
	e = (void *)round_to_cacheline(e-1);
	for(; s<=e; s += CACHE_LINE) cos_wb_cache(s);
}

#endif /* NON_CC_H */
