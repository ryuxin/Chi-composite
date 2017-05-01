#ifndef MEM_MGR_H
#define MEM_MGR_H

#include "micro_booter.h"

enum mem_mgr_captbl_layout {
	MEM_SELF_CT      = 26,
	MEM_SELF_PT      = 28,
	MEM_SELF_PMEM_PT = 30,
	MEM_COMP_PT_BASE = 32,
	MEM_CAPTBL_FREE  = round_up_to_pow2(MEM_COMP_PT_BASE+NUM_NODE*CAP32B_IDSZ, CAPMAX_ENTRY_SZ)
};

struct mem_meta {
	int size;
	int refcnt;
	vaddr_t addr;
	vaddr_t dest[NUM_NODE];
};

void mem_mgr_init(vaddr_t untype, int size, vaddr_t vas_start);
void *alloc_pages(int size);
void *alias_pages(int node, void *addr, int size);
void free_pages(void *addr, int n);
int mem_create(void *addr, int size);
void *mem_retrieve(int memid, int node);
struct mem_meta *mem_lookup(int memid);

#endif /* MEM_MGR_H */
