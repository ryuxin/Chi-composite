#include "micro_booter.h"
#include "test.h"

struct meta_header *ivshmem_meta;
int cur_node;
struct cos_compinfo booter_info;
thdcap_t termthd; 		/* switch to this to shutdown */
extern vaddr_t cos_upcall_entry;
extern int cache_op_test(void);

static void
cos_llprint(char *s, int len)
{ call_cap(PRINT_CAP_TEMP, (int)s, len, 0, 0); }

int
prints(char *s)
{
	int len = strlen(s);

	cos_llprint(s, len);

	return len;
}

int __attribute__((format(printf,1,2)))
printc(char *fmt, ...)
{
	  char s[128];
	  va_list arg_ptr;
	  int ret, len = 128;

	  va_start(arg_ptr, fmt);
	  ret = vsnprintf(s, len, fmt, arg_ptr);
	  va_end(arg_ptr);
	  cos_llprint(s, ret);

	  return ret;
}

/* For Div-by-zero test */
int num = 1, den = 0;

void
term_fn(void *d)
{ BUG_DIVZERO(); }

static sinvcap_t
boot_node_init(void)
{
	pgtblcap_t test_pt, test_untype_pt, test_pmem_pt;
	captblcap_t test_ct;
	compcap_t test_comp;
	struct cos_compinfo test_info;
	vaddr_t range, addr, src, dst;
	sinvcap_t ic;

	cur_node = 0;
	pmem_livenessid_frontier = IVSHMEM_LTBL_BASE + cur_node*IVSHMEM_LTBL_NODE_RANGE;

	range = (vaddr_t)cos_get_heap_ptr() - BOOT_MEM_VM_BASE;
	assert(range > 0);
	/* alloc test component */
	test_ct        = cos_captbl_alloc_ext(&booter_info, PMEM_TEST);
	test_pt        = cos_pgtbl_alloc_ext(&booter_info, PMEM_TEST);
	test_untype_pt = BOOT_CAPTBL_SELF_UNTYPED_PT;
	test_pmem_pt   = BOOT_CAPTBL_PMEM_PT_BASE;
	test_comp      = cos_comp_alloc_ext(&booter_info, test_ct, test_pt, (vaddr_t)&cos_upcall_entry, PMEM_TEST);
	cos_compinfo_init(&test_info, test_pt, test_ct, test_comp, (vaddr_t)BOOT_MEM_VM_BASE, TEST_FREE, &booter_info);
	printc("\tMapping in Booter's virtual memory (range:%lu) to test component\n", range);
	for (addr = 0 ; addr < range ; addr += PAGE_SIZE) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, PMEM_TEST);
		assert(src);
		memcpy((void *)src, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);
		dst = cos_mem_alias_ext(&test_info, &booter_info, src, PMEM_TEST);
		assert(dst);
	}

	/* setup capability */
	cos_cap_cpy_captbl_at(test_ct, TEST_CT, BOOT_CAPTBL_SELF_CT, test_ct);
	cos_cap_cpy_captbl_at(test_ct, TEST_PT, BOOT_CAPTBL_SELF_CT, test_pt);
	cos_cap_cpy_captbl_at(test_ct, TEST_UNTYPED_PT, BOOT_CAPTBL_SELF_CT, test_untype_pt);
	cos_cap_cpy_captbl_at(test_ct, TEST_COMP, BOOT_CAPTBL_SELF_CT, test_comp);
	cos_cap_cpy_captbl_at(test_ct, TEST_THREAD, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITTHD_BASE);
	cos_cap_cpy_captbl_at(test_ct, TEST_TCAP, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITTCAP_BASE);
	cos_cap_cpy_captbl_at(test_ct, TEST_RECV, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITRCV_BASE);
#if PMEM_TEST == 1
	cos_cap_cpy_captbl_at(test_ct, TEST_PMEM_PT, BOOT_CAPTBL_SELF_CT, test_pmem_pt);
	addr = round_up_to_pgd_page(booter_info.pmem_mi.untyped_ptr);
#else
	addr = round_up_to_pgd_page(booter_info.mi.untyped_ptr);
#endif
	dst  = round_up_to_pgd_page(dst);
	/* alloc and copy sinv capability */
	ic = cos_sinv_alloc(&booter_info, test_comp, (vaddr_t)SERVER_FN(test_start));
	call_cap_mb(ic, cur_node, addr, dst);

	return ic;
}

void
cos_init(void)
{
	while (!cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE)) ;
	printc("\t%d cycles per microsecond\n", cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE));

	cos_meminfo_init(&booter_info.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_meminfo_init(&booter_info.pmem_mi, BOOT_MEM_KM_BASE, IVSHMEM_UNTYPE_SIZE, BOOT_CAPTBL_PMEM_PT_BASE);
	cos_compinfo_init(&booter_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			  (vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, &booter_info);

	termthd = cos_thd_alloc(&booter_info, booter_info.comp_cap, term_fn, NULL);
	assert(termthd);

	printc("ct resevr %d free %d\n", TEST_RESERVE, TEST_FREE);
//	cache_op_test();
	boot_node_init();

	cos_thd_switch(termthd);

	return;
}
