#include "micro_booter.h"
#include "memcached.h"
#include "rpc.h"
#include "test.h"

#define RPC_MEM_SZ (PGD_SIZE)

enum boot_pmem_captbl_layout {
	TEST_INIT             = 2,
	BOOT_CLIENT           = 4,
	BOOT_SERVER           = 6,
	PRELOAD_KEY           = 8,
	BOOT_PMEM_CAPTBL_FREE = round_up_to_pow2(PRELOAD_KEY, CAPMAX_ENTRY_SZ)
};

struct meta_header *ivshmem_meta;
int cur_node;
struct cos_compinfo booter_info;
thdcap_t termthd; 		/* switch to this to shutdown */
__attribute__((weak)) char _binary_preload_key_start = 0;
__attribute__((weak)) char _binary_preload_key_size  = 0;
__attribute__((weak)) char _binary_preload_key_end  = 0;

__attribute__((weak)) char _binary_trace_key_start = 0;
__attribute__((weak)) char _binary_trace_key_size  = 0;
__attribute__((weak)) char _binary_trace_key_end  = 0;


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

static inline void
fork_comp(struct cos_compinfo *comp)
{
	vaddr_t range, addr, src, dst, s, e;

	range = (vaddr_t)cos_get_heap_ptr() - BOOT_MEM_VM_BASE;
	assert(range > 0);
	s = (vaddr_t)round_up_to_page(&_binary_preload_key_start) - BOOT_MEM_VM_BASE;
	e = (vaddr_t)round_to_page(&_binary_trace_key_end) - BOOT_MEM_VM_BASE;
	for (addr = 0 ; addr < s ; addr += PAGE_SIZE) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, 1);
		assert(src);
		memcpy((void *)src, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);
		dst = cos_mem_alias_ext(comp, &booter_info, src, 1);
		assert(dst);
	}
	/* do not copy trace file to each component. it is too large. */
	for (; addr <= e ; addr += PAGE_SIZE) {
		dst = cos_mem_alias_ext(comp, &booter_info, BOOT_MEM_VM_BASE + addr, 1);
		assert(dst);
	}
	for (; addr < range ; addr += PAGE_SIZE) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, 1);
		assert(src);
		memcpy((void *)src, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);
		dst = cos_mem_alias_ext(comp, &booter_info, src, 1);
		assert(dst);
	}
}

static sinvcap_t
non_boot_node_init(void)
{
	struct cos_compinfo temp;
	sinvcap_t ret;

	while (!ivshmem_meta->boot_done) { ; }
	cur_node = cos_faa(&ivshmem_meta->boot_num, 1);
	pmem_livenessid_frontier = IVSHMEM_LTBL_BASE + cur_node*IVSHMEM_LTBL_NODE_RANGE;
	temp.captbl_cap = BOOT_CAPTBL_PMEM_CT_BASE;

	if (cur_node < NUM_NODE/2) ret = cos_cap_cpy(&booter_info, &temp, CAP_SINV, BOOT_SERVER);
	else {
		ret = cos_cap_cpy(&booter_info, &temp, CAP_SINV, TEST_INIT);
		call_cap_mb(ret, cur_node, 2, 3);
		ret = cos_cap_cpy(&booter_info, &temp, CAP_SINV, BOOT_CLIENT);
	}

	return ret;
}

static sinvcap_t
boot_node_init(void)
{
	pgtblcap_t client_pt[NUM_NODE], rpc_pt, rpc_pmem_pt, mc_pt, mc_pmem_pt;
	captblcap_t client_ct[NUM_NODE], rpc_ct, mc_ct;
	compcap_t client_comp[NUM_NODE], rpc_comp, mc_comp;
	struct cos_compinfo client_info[NUM_NODE], rpc_info, mc_info;
	vaddr_t addr, src, dst;
	sinvcap_t ic, ret_ic;
	int i, j;

	cur_node = 0;
	pmem_livenessid_frontier = IVSHMEM_LTBL_BASE + cur_node*IVSHMEM_LTBL_NODE_RANGE;
	assert(&_binary_trace_key_end > &_binary_preload_key_start);
	assert(&_binary_trace_key_start == &_binary_preload_key_end);
	load_key = &_binary_preload_key_start;
	ops = &_binary_trace_key_start;

	for(i=NUM_NODE/2; i<NUM_NODE; i++) client_pt[i] = cos_pgtbl_alloc_ext(&booter_info, 1);
	rpc_pt = cos_pgtbl_alloc_ext(&booter_info, 1);
	mc_pt = cos_pgtbl_alloc_ext(&booter_info, 1);
	/* alloc client component */
	for(i=NUM_NODE/2; i<NUM_NODE; i++) {
		client_ct[i]   = cos_captbl_alloc_ext(&booter_info, 1);
		client_comp[i] = cos_comp_alloc_ext(&booter_info, client_ct[i], client_pt[i], (vaddr_t)NULL, 1);
		cos_compinfo_init(&client_info[i], client_pt[i], client_ct[i], client_comp[i], (vaddr_t)BOOT_MEM_VM_BASE, RPC_CAPTBL_FREE, &booter_info);
		fork_comp(&client_info[i]);
	}

	/* alloc rpc/mem_mgr component */
	rpc_ct = cos_captbl_alloc_ext(&booter_info, 1);
	rpc_pmem_pt = BOOT_CAPTBL_PMEM_PT_BASE;
	rpc_comp    = cos_comp_alloc_ext(&booter_info, rpc_ct, rpc_pt, (vaddr_t)NULL, 1);
	cos_compinfo_init(&rpc_info, rpc_pt, rpc_ct, rpc_comp, (vaddr_t)BOOT_MEM_VM_BASE, MEM_CAPTBL_FREE, &booter_info);
	fork_comp(&rpc_info);

	/* alloc memcached component */
	mc_ct = cos_captbl_alloc_ext(&booter_info, 1);
	mc_pmem_pt = BOOT_CAPTBL_PMEM_PT_BASE;
	mc_comp    = cos_comp_alloc_ext(&booter_info, mc_ct, mc_pt, (vaddr_t)NULL, 1);
	cos_compinfo_init(&mc_info, mc_pt, mc_ct, mc_comp, (vaddr_t)BOOT_MEM_VM_BASE, MEM_CAPTBL_FREE, &booter_info);
	fork_comp(&mc_info);

	/* setup capability */
	/* alloc and copy sinv capability */
	/* copy sinv cap to other node booter's pmem cap_tbl
	 * first NUM_NODE/2 are servers and second NUM_NODE/2 are clients */
	ret_ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_server_start));
	cos_cap_cpy_captbl_at(BOOT_CAPTBL_PMEM_CT_BASE+2*CAP32B_IDSZ, BOOT_SERVER, BOOT_CAPTBL_SELF_CT, ret_ic);
	for(i=NUM_NODE/2; i<NUM_NODE; i++) {
		ic = cos_sinv_alloc(&booter_info, client_comp[i], (vaddr_t)SERVER_FN(client_init));
		cos_cap_cpy_captbl_at(BOOT_CAPTBL_PMEM_CT_BASE+(i+1)*CAP32B_IDSZ, TEST_INIT, BOOT_CAPTBL_SELF_CT, ic);
		ic = cos_sinv_alloc(&booter_info, client_comp[i], (vaddr_t)SERVER_FN(client_start));
		cos_cap_cpy_captbl_at(BOOT_CAPTBL_PMEM_CT_BASE+(i+1)*CAP32B_IDSZ, BOOT_CLIENT, BOOT_CAPTBL_SELF_CT, ic);
	}

	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_create));
	cos_cap_cpy_captbl_at(mc_ct, RPC_CREATE, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_connect));
	cos_cap_cpy_captbl_at(mc_ct, RPC_CONNECT, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_send));
	cos_cap_cpy_captbl_at(mc_ct, RPC_SEND, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_recv));
	cos_cap_cpy_captbl_at(mc_ct, RPC_RECV, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_free));
	cos_cap_cpy_captbl_at(mc_ct, RPC_FREE, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_register));
	cos_cap_cpy_captbl_at(mc_ct, RPC_REGISTER, BOOT_CAPTBL_SELF_CT, ic);

	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_register));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_REGISTER, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_set_key));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_SET_KEY, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_get_key));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_GET_KEY, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_init));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_INIT, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_print_status));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_PRINT_STATUS, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_hashtbl_flush));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_FLUSH, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_disconnect));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_DISCONNECT, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_preload_key));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_PRELOAD, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_test));
	for(i=NUM_NODE/2; i<NUM_NODE; i++) cos_cap_cpy_captbl_at(client_ct[i], MC_TEST, BOOT_CAPTBL_SELF_CT, ic);
	/* copy page table and its own cap_tbl cap to mem_mgr */
	cos_cap_cpy_captbl_at(rpc_ct, MEM_SELF_CT, BOOT_CAPTBL_SELF_CT, rpc_ct);
	cos_cap_cpy_captbl_at(rpc_ct, MEM_SELF_PT, BOOT_CAPTBL_SELF_CT, rpc_pt);
	cos_cap_cpy_captbl_at(rpc_ct, MEM_SELF_PMEM_PT, BOOT_CAPTBL_SELF_CT, rpc_pmem_pt);
	for(i=0; i<NUM_NODE/2; i++) {
		cos_cap_cpy_captbl_at(rpc_ct, MEM_COMP_PT_BASE+i*CAP32B_IDSZ, BOOT_CAPTBL_SELF_CT, mc_pt);
	}
	for(; i<NUM_NODE; i++) {
		cos_cap_cpy_captbl_at(rpc_ct, MEM_COMP_PT_BASE+i*CAP32B_IDSZ, BOOT_CAPTBL_SELF_CT, client_pt[i]);
	}
	cos_cap_cpy_captbl_at(mc_ct, MEM_SELF_CT, BOOT_CAPTBL_SELF_CT, mc_ct);
	cos_cap_cpy_captbl_at(mc_ct, MEM_SELF_PT, BOOT_CAPTBL_SELF_CT, mc_pt);
	cos_cap_cpy_captbl_at(mc_ct, MEM_SELF_PMEM_PT, BOOT_CAPTBL_SELF_CT, mc_pmem_pt);
	for(i=NUM_NODE/2; i<NUM_NODE; i++) {
		cos_cap_cpy_captbl_at(mc_ct, MEM_COMP_PT_BASE+i*CAP32B_IDSZ, BOOT_CAPTBL_SELF_CT, client_pt[i]);
	}
	/* alias shared meta page to each component*/
	for(i=NUM_NODE/2; i<NUM_NODE; i++) {
		dst = cos_mem_alias_ext(&client_info[i], &booter_info, (vaddr_t)ivshmem_meta, 1);
		assert(dst == (vaddr_t)ivshmem_meta);
	}
	cos_mem_alias_at(&rpc_info, (vaddr_t)ivshmem_meta, &booter_info, (vaddr_t)ivshmem_meta);
	cos_mem_alias_at(&mc_info, (vaddr_t)ivshmem_meta, &booter_info, (vaddr_t)ivshmem_meta);

	for(i=0; i<COST_ARRAY_NUM_PAGE; i++) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, 0);
		assert(src);
		for(j=NUM_NODE/2; j<NUM_NODE; j++) {
			dst = cos_mem_alias_ext(&client_info[j], &booter_info, src, 1);
			assert(dst);
		}
	}

	/* give 4M to booter self, 4M to rpc and the rest to memcached */
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_init));
	addr = round_up_to_pgd_page(booter_info.pmem_mi.untyped_ptr);
	call_cap_mb(ic, cur_node, addr, RPC_MEM_SZ);
	ic = cos_sinv_alloc(&booter_info, mc_comp, (vaddr_t)SERVER_FN(mc_init));
	addr += RPC_MEM_SZ;
	call_cap_mb(ic, cur_node, addr, booter_info.pmem_mi.untyped_frontier-addr);
	ivshmem_meta->boot_done = 1;

	return ret_ic;
}

void
cos_init(void)
{
	sinvcap_t inv_ic;
	int ret;

	while (!cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE)) {
		int i = 0x100000;
		while (i > 0) i --;
	}
	printc("\t%d cycles per microsecond\n", cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE));

	cos_meminfo_init(&booter_info.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_meminfo_init(&booter_info.pmem_mi, BOOT_MEM_KM_BASE, IVSHMEM_UNTYPE_SIZE, BOOT_CAPTBL_PMEM_PT_BASE);
	cos_compinfo_init(&booter_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			  (vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, &booter_info);
	ivshmem_meta = (struct meta_header *)cos_mem_alias_pmem(&booter_info, &booter_info, 0, 0);
	printc("meta %x magic %s boot %d done %d\n", ivshmem_meta, ivshmem_meta->magic, ivshmem_meta->boot_num, ivshmem_meta->boot_done);

	termthd = cos_thd_alloc(&booter_info, booter_info.comp_cap, term_fn, NULL);
	assert(termthd);

	ret = cos_cas((unsigned long *)&ivshmem_meta->boot_num, 0, 1);
	if (ret != CAS_SUCCESS) inv_ic = non_boot_node_init();
	else inv_ic = boot_node_init();
	call_cap_mb(inv_ic, cur_node, 2, 3);

	cos_thd_switch(termthd);

	return;
}
