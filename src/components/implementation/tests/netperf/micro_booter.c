#include "micro_booter.h"
#include "rpc.h"
#include "bench.h"

enum boot_pmem_captbl_layout {
	BOOT_CLIENT           = 2,
	BOOT_SERVER           = 4,
	BOOT_PMEM_CAPTBL_FREE = round_up_to_pow2(BOOT_SERVER, CAPMAX_ENTRY_SZ)
};

struct meta_header *ivshmem_meta;
int cur_node;
struct cos_compinfo booter_info;
thdcap_t termthd; 		/* switch to this to shutdown */

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
non_boot_node_init(void)
{
	struct cos_compinfo temp;
	sinvcap_t ret;

	while (!ivshmem_meta->boot_done) { ; }
	cur_node = ivshmem_meta->boot_num++;
	pmem_livenessid_frontier = IVSHMEM_LTBL_BASE + cur_node*IVSHMEM_LTBL_NODE_RANGE;
	temp.captbl_cap = BOOT_CAPTBL_PMEM_CT_BASE;
	ret = cos_cap_cpy(&booter_info, &temp, CAP_SINV, BOOT_SERVER);

	return ret;
}

static sinvcap_t
boot_node_init(void)
{
	pgtblcap_t client_pt, server_pt, rpc_pt, rpc_pmem_pt;
	captblcap_t client_ct, server_ct, rpc_ct;
	compcap_t client_comp, server_comp, rpc_comp;
	struct cos_compinfo client_info, server_info, rpc_info;
	vaddr_t range, addr, src, dst;
	sinvcap_t ic, client_ic;

	cur_node = 0;
	ivshmem_meta->boot_num = 1;
	pmem_livenessid_frontier = IVSHMEM_LTBL_BASE + cur_node*IVSHMEM_LTBL_NODE_RANGE;

	range = (vaddr_t)cos_get_heap_ptr() - BOOT_MEM_VM_BASE;
	assert(range > 0);
	/* alloc client component */
	client_ct = cos_captbl_alloc_ext(&booter_info, 1);
	client_pt = cos_pgtbl_alloc_ext(&booter_info, 1);
	client_comp    = cos_comp_alloc_ext(&booter_info, client_ct, client_pt, (vaddr_t)NULL, 1);
	cos_compinfo_init(&client_info, client_pt, client_ct, client_comp, (vaddr_t)BOOT_MEM_VM_BASE, RPC_CAPTBL_FREE, &booter_info);
	printc("\tMapping in Booter's virtual memory (range:%lu) to client component\n", range);
	for (addr = 0 ; addr < range ; addr += PAGE_SIZE) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, 1);
		assert(src);
		memcpy((void *)src, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);
		dst = cos_mem_alias_ext(&client_info, &booter_info, src, 1);
		assert(dst);
	}

	/* alloc server component */
	server_ct = cos_captbl_alloc_ext(&booter_info, 1);
	server_pt = cos_pgtbl_alloc_ext(&booter_info, 1);
	server_comp    = cos_comp_alloc_ext(&booter_info, server_ct, server_pt, (vaddr_t)NULL, 1);
	cos_compinfo_init(&server_info, server_pt, server_ct, server_comp, (vaddr_t)BOOT_MEM_VM_BASE, RPC_CAPTBL_FREE, &booter_info);
	printc("\tMapping in Booter's virtual memory (range:%lu) to server component\n", range);
	for (addr = 0 ; addr < range ; addr += PAGE_SIZE) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, 1);
		assert(src);
		memcpy((void *)src, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);
		dst = cos_mem_alias_ext(&server_info, &booter_info, src, 1);
		assert(dst);
	}

	/* alloc rpc/mem_mgr component */
	rpc_ct = cos_captbl_alloc_ext(&booter_info, 1);
	rpc_pt = cos_pgtbl_alloc_ext(&booter_info, 1);
	rpc_pmem_pt = BOOT_CAPTBL_PMEM_PT_BASE;
	rpc_comp    = cos_comp_alloc_ext(&booter_info, rpc_ct, rpc_pt, (vaddr_t)NULL, 1);
	cos_compinfo_init(&rpc_info, rpc_pt, rpc_ct, rpc_comp, (vaddr_t)BOOT_MEM_VM_BASE, MEM_CAPTBL_FREE, &booter_info);
	printc("\tMapping in Booter's virtual memory (range:%lu) to rpc component\n", range);
	for (addr = 0 ; addr < range ; addr += PAGE_SIZE) {
		src = (vaddr_t)cos_page_bump_alloc_ext(&booter_info, 1);
		assert(src);
		memcpy((void *)src, (void *)(BOOT_MEM_VM_BASE + addr), PAGE_SIZE);
		dst = cos_mem_alias_ext(&rpc_info, &booter_info, src, 1);
		assert(dst);
	}

	/* setup capability */
	/* alloc and copy sinv capability */
	ic = cos_sinv_alloc(&booter_info, server_comp, (vaddr_t)SERVER_FN(server_start));
	/* copy sinv cap to other node booter's pmem cap_tbl*/
	cos_cap_cpy_captbl_at(BOOT_CAPTBL_PMEM_CT_BASE+2*CAP32B_IDSZ, BOOT_SERVER, BOOT_CAPTBL_SELF_CT, ic);
	client_ic = cos_sinv_alloc(&booter_info, client_comp, (vaddr_t)SERVER_FN(client_start));
	cos_cap_cpy_captbl_at(BOOT_CAPTBL_PMEM_CT_BASE+2*CAP32B_IDSZ, BOOT_CLIENT, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_create));
	cos_cap_cpy_captbl_at(client_ct, RPC_CREATE, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_CREATE, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_connect));
	cos_cap_cpy_captbl_at(client_ct, RPC_CONNECT, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_CONNECT, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_send));
	cos_cap_cpy_captbl_at(client_ct, RPC_SEND, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_SEND, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_recv));
	cos_cap_cpy_captbl_at(client_ct, RPC_RECV, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_RECV, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_free));
	cos_cap_cpy_captbl_at(client_ct, RPC_FREE, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_FREE, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_register));
	cos_cap_cpy_captbl_at(client_ct, RPC_REGISTER, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_REGISTER, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_call));
	cos_cap_cpy_captbl_at(client_ct, RPC_CALL, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_CALL, BOOT_CAPTBL_SELF_CT, ic);
	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_wait_replay));
	cos_cap_cpy_captbl_at(client_ct, RPC_WAIT, BOOT_CAPTBL_SELF_CT, ic);
	cos_cap_cpy_captbl_at(server_ct, RPC_WAIT, BOOT_CAPTBL_SELF_CT, ic);
	/* copy page table cap to mem_mgr */
	cos_cap_cpy_captbl_at(rpc_ct, MEM_SELF_CT, BOOT_CAPTBL_SELF_CT, rpc_ct);
	cos_cap_cpy_captbl_at(rpc_ct, MEM_SELF_PT, BOOT_CAPTBL_SELF_CT, rpc_pt);
	cos_cap_cpy_captbl_at(rpc_ct, MEM_SELF_PMEM_PT, BOOT_CAPTBL_SELF_CT, rpc_pmem_pt);
	cos_cap_cpy_captbl_at(rpc_ct, MEM_COMP_PT_BASE, BOOT_CAPTBL_SELF_CT, client_pt);
	cos_cap_cpy_captbl_at(rpc_ct, MEM_COMP_PT_BASE+CAP32B_IDSZ, BOOT_CAPTBL_SELF_CT, server_pt);
	/* alias shared meta page to each component*/
	cos_mem_alias_at(&client_info, (vaddr_t)ivshmem_meta, &booter_info, (vaddr_t)ivshmem_meta);
	cos_mem_alias_at(&server_info, (vaddr_t)ivshmem_meta, &booter_info, (vaddr_t)ivshmem_meta);
	cos_mem_alias_at(&rpc_info, (vaddr_t)ivshmem_meta, &booter_info, (vaddr_t)ivshmem_meta);

	ic = cos_sinv_alloc(&booter_info, rpc_comp, (vaddr_t)SERVER_FN(rpc_init));
	addr = round_up_to_pgd_page(booter_info.pmem_mi.untyped_ptr);
	call_cap_mb(ic, cur_node, addr, booter_info.pmem_mi.untyped_frontier-addr);
	ivshmem_meta->boot_done = 1;

	return client_ic;
}

void
cos_init(void)
{
	sinvcap_t inv_ic;

	while (!cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE)) ;
	printc("\t%d cycles per microsecond\n", cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE));

	cos_meminfo_init(&booter_info.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_meminfo_init(&booter_info.pmem_mi, BOOT_MEM_KM_BASE, IVSHMEM_UNTYPE_SIZE, BOOT_CAPTBL_PMEM_PT_BASE);
	cos_compinfo_init(&booter_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			  (vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, &booter_info);
	ivshmem_meta = (struct meta_header *)cos_mem_alias_pmem(&booter_info, &booter_info, 0, 0);
	printc("meta %x magic %s done %d\n", ivshmem_meta, ivshmem_meta->magic, ivshmem_meta->boot_done);

	termthd = cos_thd_alloc(&booter_info, booter_info.comp_cap, term_fn, NULL);
	assert(termthd);

	if (ivshmem_meta->boot_num) inv_ic = non_boot_node_init();
	else inv_ic = boot_node_init();
	call_cap_mb(inv_ic, cur_node, 2, 3);

	cos_thd_switch(termthd);

	return;
}
