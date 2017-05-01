#include "include/non_CC.h"
#include "include/pgtbl.h"

#define TOT_PTE_NUM (PAGE_SIZE/sizeof(unsigned long))
#define CC_PTE_NUM  (TOT_PTE_NUM/2)
struct non_cc_quiescence *cc_quiescence;
u64_t local_quiescence[NUM_NODE];
u64_t *global_tsc, clflush_start;
extern int ivshmem_pgd_idx, ivshmem_pgd_end;
extern u32_t *boot_comp_pgd;
int cur_pgd_idx, cur_pte_idx, npage_flush;

static inline int
__cc_quiescence_local_check(u64_t timestamp)
{
	int i, quiescent = 1;
	for (i = (cur_node+1)%NUM_NODE; i != cur_node; i = (i+1)%NUM_NODE) {
		if (timestamp > local_quiescence[i]) {
			quiescent = 0;
			break;
		}
	}
	return quiescent;
}

static inline void
copy_cc_quiescence(void)
{
	int i;

	for(i=0; i<NUM_NODE; i++) cos_flush_cache(&cc_quiescence[i].last_mandatory_flush);
	for(i=0; i<NUM_NODE; i++) local_quiescence[i] = cc_quiescence[i].last_mandatory_flush;
}

static int
non_cc_quiescence_check(u64_t timestamp)
{
	if (!__cc_quiescence_local_check(timestamp)) {
		copy_cc_quiescence();
	}
	return __cc_quiescence_local_check(timestamp);
}

int
cos_quiescence_check(u64_t cur, u64_t past, u64_t grace_period, quiescence_type_t type)
{
	switch(type) {
	case TLB_QUIESCENCE:
		return tlb_quiescence_check(past);
	case KERNEL_QUIESCENCE:
		return QUIESCENCE_CHECK(cur, past, grace_period);
	case NON_CC_QUIESCENCE:
		return non_cc_quiescence_check(past);
	}
	return 0;
}

static inline void
global_tlb_flush(void)
{
	u32_t cr3, cr4;

	asm("movl %%cr4, %0" : "=r"(cr4));
	asm("movl %%cr3, %0" : "=r"(cr3));
	asm("movl %0, %%cr4" : : "r"(cr4 & (~(1<<7)) ));
	asm("movl %0, %%cr3" : : "r"(cr3));
	asm("movl %0, %%cr4" : : "r"(cr4));
}

static inline int
global_pte_flush(void)
{
//#define IVSHMEM_RETYPE_SZ (IVSHMEM_TOT_SIZE/(RETYPE_MEM_NPAGES*PAGE_SIZE))
#define IVSHMEM_RETYPE_SZ (*max_pmem_idx)
#define KERNEL_OFF 768
	static u32_t off=0;
	unsigned long *pgd, pte;
	void *addr;
	int i;

	for(; off<IVSHMEM_RETYPE_SZ; off++) {
		if (!pmem_retype_tbl->mem_set[off].__pad) continue;
		addr = (void *)ivshmem_phy_addr+off*RETYPE_MEM_SIZE;
		pgd  = (unsigned long *)chal_pa2va((paddr_t)addr);
		for(i=0; i<KERNEL_OFF; i++) {
			pte = pgd[i];
			if (pte & PGTBL_PRESENT) {
				addr = chal_pa2va(pte & PGTBL_FRAME_MASK);
				cos_clflush_range(addr, addr+PAGE_SIZE);
				npage_flush++;
			}
		}
		off++;
		return 1;
	}
	off = 0;
	return 0;
}

int
cos_cache_mandatory_flush(void)
{
	unsigned long *pte, page;
	void *addr;
	int i, j, r = -1;
	u32_t *kernel_pgtbl = (u32_t *)&boot_comp_pgd;

	if (cur_pgd_idx == ivshmem_pgd_idx && !cur_pte_idx) {
		if (!npage_flush) non_cc_rdtscll(&clflush_start);
		if (global_pte_flush()) return -1;
	}
	if (cur_pgd_idx == ivshmem_pgd_end) {
		cc_quiescence[cur_node].last_mandatory_flush = clflush_start;
		cos_wb_cache(&cc_quiescence[cur_node].last_mandatory_flush);
		r = npage_flush;
		cur_pgd_idx = ivshmem_pgd_idx;
		cur_pte_idx = 0;
		global_tlb_flush();
		npage_flush = 0;
		asm volatile ("sfence"); /* serialize */
		return r;
	}

	page = kernel_pgtbl[cur_pgd_idx];
	pte = chal_pa2va(page & PGTBL_FRAME_MASK);
	for(i=cur_pte_idx; i < cur_pte_idx+(int)CC_PTE_NUM; i++) {
		page = pte[i];
		if (page & PGTBL_ACCESSED) {
			addr = chal_pa2va(page & PGTBL_FRAME_MASK);
			cos_clflush_range(addr, addr+PAGE_SIZE);
			pte[i] &= (~PGTBL_ACCESSED);
			npage_flush++;
		}
	}
	cur_pte_idx += CC_PTE_NUM;
	if (cur_pte_idx == TOT_PTE_NUM) {
		cur_pte_idx = 0;
		cur_pgd_idx++;
	}

	return -1;
}

void
non_cc_init(void)
{
	cur_pgd_idx = ivshmem_pgd_idx;
	cur_pte_idx = 0;
	npage_flush = 0;
}
