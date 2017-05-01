#include "ivshmem.h"
#include "string.h"
#include "chal.h"

paddr_t ivshmem_phy_addr;
size_t ivshmem_sz;
vaddr_t ivshmem_addr=0, ivshmem_bump;
struct ivshmem_meta *meta_page;
int cur_node, ivshmem_pgd_idx, ivshmem_pgd_end;
extern u8_t *boot_comp_pgd;
extern int boot_nptes(unsigned int sz);

u8_t *
ivshmem_boot_alloc(unsigned int size)
{
	assert(ivshmem_bump < ivshmem_addr+IVSHMEM_UNTYPE_START);
	u8_t *r = (u8_t *)ivshmem_bump;
	size = round_to_page(size);
	ivshmem_bump += size;
	return r;
}

void
clear_bump_alloc(void)
{
	unsigned long i = (unsigned long)(round_up_to_pow2(ivshmem_bump, PAGE_SIZE * RETYPE_MEM_NPAGES));

	for(; i < (unsigned long)(ivshmem_addr+IVSHMEM_UNTYPE_START) ; 
		 i += PAGE_SIZE * RETYPE_MEM_NPAGES) {
		if (retypetbl_retype2frame((void*)chal_va2pa((void*)i))) {
				die("Retyping back to frame on ivshmem heap allocation failed @ 0x%x.\n", i);
		}
	}
}

int
ivshmem_pgtbl_mappings_add(struct captbl *ct, capid_t pgdcap, capid_t ptecap, const char *label,
			void *kern_vaddr, unsigned long user_vaddr, unsigned int range, int uvm)
{
	int ret;
	u8_t *ptes;
	unsigned int nptes = 0, i;
	struct cap_pgtbl *pte_cap, *pgd_cap;
	pgtbl_t pgtbl;

	pgd_cap = (struct cap_pgtbl*)captbl_lkup(ct, pgdcap);
	if (!pgd_cap || !CAP_TYPECHK(pgd_cap, CAP_PGTBL)) assert(0);
	pgtbl = (pgtbl_t)pgd_cap->pgtbl;
	nptes = boot_nptes(round_up_to_pgd_page(user_vaddr+range)-round_to_pgd_page(user_vaddr));
	ptes = ivshmem_boot_alloc(nptes*PAGE_SIZE);
	assert(ptes);
	printk("\tCreating %d %s PTEs for PGD @ 0x%x from [%x,%x) to [%x,%x).\n",
	       nptes, label, chal_pa2va((paddr_t)pgtbl),
	       kern_vaddr, kern_vaddr+range, user_vaddr, user_vaddr+range);

	pte_cap = (struct cap_pgtbl*)captbl_lkup(ct, ptecap);
	assert(pte_cap);

	/* Hook in the PTEs */
	for (i = 0 ; i < nptes ; i++) {
		u8_t   *p  = ptes + i * PAGE_SIZE;

		pgtbl_init_pte(p);
		pte_cap->pgtbl = (pgtbl_t)p;

		/* hook the pte into the boot component's page tables */
		ret = cap_cons(ct, pgdcap, ptecap, (capid_t)(user_vaddr + i*PGD_RANGE));
		assert(!ret);
	}

	printk("\tMapping in %s.\n", label);
	/* Map in the actual memory. */
	for (i = 0 ; i < range/PAGE_SIZE ; i++) {
		u8_t *p     = kern_vaddr + i * PAGE_SIZE;
		paddr_t pf  = chal_va2pa(p);
		u32_t mapat = (u32_t)user_vaddr + i * PAGE_SIZE, flags = 0;

		if (uvm  && pgtbl_mapping_add(pgtbl, mapat, pf, PGTBL_USER_DEF)) assert(0);
		if (!uvm && pgtbl_cosframe_add(pgtbl, mapat, pf, PGTBL_COSFRAME)) assert(0);
		assert((void*)p == pgtbl_lkup(pgtbl, user_vaddr+i*PAGE_SIZE, &flags));
	}

	return 0;
}


void
ivshmem_set_page(u32_t page)
{
	assert(sizeof(struct ivshmem_meta) <= PAGE_SIZE);
	ivshmem_pgd_idx = (int)page;
	ivshmem_pgd_end = page+ivshmem_sz/PGD_RANGE;
	ivshmem_addr = page * (1 << 22);
	meta_page    = (struct ivshmem_meta *)ivshmem_addr;
	ivshmem_bump = ivshmem_addr+PAGE_SIZE * RETYPE_MEM_NPAGES;
	/* printk("ivshmem phy %x virtul %x sz %d bump %x\n", ivshmem_phy_addr, ivshmem_addr, ivshmem_sz, ivshmem_bump); */
}

void
ivshmem_boot_init(struct captbl *ct)
{
	int j, nkmemptes, ret = 0;
	unsigned long int i;
	pgtbl_t pgtbl;
	u8_t *captbl;
	struct captbl *pmem_ct;

	non_cc_init();
	ret = cos_cas((unsigned long *)&meta_page->node_num, 0, 1);
	if (meta_page->node_num >= NUM_NODE) meta_page->kernel_done = 0;
	if (ret != CAS_SUCCESS) {
		while (!meta_page->kernel_done) { ; }
		cur_node = cos_faa(&meta_page->node_num, 1);
		if (pgtbl_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_PMEM_PT_BASE, meta_page->pmem_pgd[cur_node], 0)) assert(0);
		if (captbl_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_PMEM_CT_BASE, meta_page->pmem_ct[cur_node], 0)) assert(0);
		__pmem_liveness_tbl = meta_page->pmem_liveness_tbl;
		pmem_retype_tbl     = meta_page->pmem_retype_tbl;
		max_pmem_idx        = &(meta_page->max_pmem_idx);
		cc_quiescence       = meta_page->pmem_cc_quiescence;
		global_tsc          = &meta_page->global_tsc;
		printk("node %d ivshmem kernel done!\n", cur_node);
		return ;
	}
	cur_node = 0;
	meta_page->kernel_done = 0;
	printk("ivshmem phy (%x, %x), vir (%x, %x)\n", ivshmem_phy_addr, ivshmem_phy_addr+IVSHMEM_TOT_SIZE, ivshmem_addr, ivshmem_addr+IVSHMEM_TOT_SIZE);

	meta_page->pmem_liveness_tbl = (struct liveness_entry *)ivshmem_boot_alloc(sizeof(__liveness_tbl));
	ltbl_init(meta_page->pmem_liveness_tbl);
	__pmem_liveness_tbl = meta_page->pmem_liveness_tbl;

	meta_page->pmem_retype_tbl = (struct retype_info *)ivshmem_boot_alloc(sizeof(struct retype_info));

	for (j = 0; j < N_MEM_SETS; j++) {
		meta_page->pmem_retype_tbl->mem_set[j].refcnt_atom.type    = RETYPETBL_UNTYPED;
		meta_page->pmem_retype_tbl->mem_set[j].refcnt_atom.ref_cnt = 0;
		meta_page->pmem_retype_tbl->mem_set[j].last_unmap          = 0;
	}
	max_pmem_idx = &(meta_page->max_pmem_idx);
	pmem_retype_tbl = meta_page->pmem_retype_tbl;
	retypetbl_retype2user((void*)chal_va2pa((void*)ivshmem_addr));
	for (i = (unsigned long)ivshmem_addr+PAGE_SIZE * RETYPE_MEM_NPAGES ; 
		 i < (unsigned long)(ivshmem_addr+IVSHMEM_UNTYPE_START) ; 
		 i += PAGE_SIZE * RETYPE_MEM_NPAGES) {
		if (retypetbl_retype2kern((void*)chal_va2pa((void*)i))) {
				die("Retyping to kernel on ivshmem heap allocation failed @ 0x%x.\n", i);
		}
	}

	cc_quiescence = meta_page->pmem_cc_quiescence;
	memset(cc_quiescence, 0, sizeof(struct non_cc_quiescence));

	global_tsc = &meta_page->global_tsc;

	for (i = 1; i <= NUM_NODE; i++) {
		pgtbl = (pgtbl_t)ivshmem_boot_alloc(PAGE_SIZE);
		memset(pgtbl, 0, PAGE_SIZE);
		memcpy((void *)pgtbl + KERNEL_PGD_REGION_OFFSET, (void *)&boot_comp_pgd + KERNEL_PGD_REGION_OFFSET, KERNEL_PGD_REGION_SIZE);
		pgtbl = (pgtbl_t)chal_va2pa(pgtbl);
		meta_page->pmem_pgd[i-1] = pgtbl;
		if (pgtbl_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_PMEM_PT_BASE+i*CAP32B_IDSZ, pgtbl, 0)) assert(0);

		ret = ivshmem_pgtbl_mappings_add(ct, BOOT_CAPTBL_PMEM_PT_BASE+i*CAP32B_IDSZ, BOOT_CAPTBL_KM_PTE, "shared meta page", 
				(void *)ivshmem_addr, 0, PAGE_SIZE, 1);
		assert(ret == 0);
	}

	/* give all untyped memory to the booter node's booter */
	ret = ivshmem_pgtbl_mappings_add(ct, BOOT_CAPTBL_PMEM_PT_BASE+CAP32B_IDSZ, BOOT_CAPTBL_KM_PTE, "untyped global memory", 
			(void *)ivshmem_addr+IVSHMEM_UNTYPE_START, BOOT_MEM_KM_BASE, IVSHMEM_UNTYPE_SIZE, 0);
	assert(ret == 0);

	for (i = 1; i <= NUM_NODE; i++) {
		captbl = (u8_t *)ivshmem_boot_alloc(PAGE_SIZE);
		pmem_ct = captbl_create(captbl);
		meta_page->pmem_ct[i-1] = pmem_ct;
		ret = captbl_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_PMEM_CT_BASE+i*CAP32B_IDSZ, pmem_ct, 0);
		assert(ret == 0);
	}

	if (pgtbl_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_PMEM_PT_BASE, meta_page->pmem_pgd[cur_node], 0)) assert(0);
	if (captbl_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_PMEM_CT_BASE, meta_page->pmem_ct[cur_node], 0)) assert(0);
	clear_bump_alloc();

	memcpy(meta_page->magic, IVSHMEM_MAGIC, MAGIC_LEN);
	meta_page->kernel_done = 1;
}
