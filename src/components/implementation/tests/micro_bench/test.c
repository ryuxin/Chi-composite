#include "test.h"

#define ITER 10000000
//#define ITER 100000
#define KERNEL_PAGE 300
#define OFFSET 5
static struct cos_compinfo test_info;
static int act[ITER], deact[ITER];
volatile arcvcap_t rcc_global, rcp_global;
volatile asndcap_t scp_global;
int async_test_flag = 0;

static int
delay(unsigned long long cycles)
{
	return 0;
	unsigned long long s,e;
	volatile int mem = 0;

	rdtscll(s);
	while (1) {
		rdtscll(e);
		if (e - s > cycles) break;
		mem++;
	}

	return 0;
}

static int cmpfunc(const void * a, const void * b)
{
    return ( *(int*)b - *(int*)a );
}

static void
stddev(int *re, char *label)
{
	unsigned long long sum = 0, dev = 0;
	int max = 0, i, avg;

	qsort(re, ITER, sizeof(int), cmpfunc);
	for(i=0; i<ITER; i++) {
		sum += (unsigned long long)re[i];
		if (re[i] > max) max = re[i];
	}
	avg = sum/ITER;
/*	for(i=0; i<ITER; i++) {
		dev += (unsigned long long)(re[i]-avg)*(re[i]-avg);
	}
	for(i=0; i<10; i++) printc("%d ", re[i]);
	printc("\n");*/
	printc("%s iter %d avg %d 99th %d %d %d\n", label, ITER, avg, re[ITER/100], re[ITER/1000], re[ITER/10000]);
}

static void
cap_act_test(void)
{
	unsigned long long s, e, t1, t2;
	int i, ret, lid;

	lid = livenessid_bump_alloc(PMEM_TEST);
	t1 = t2 = 0;

	for(i=0; i<ITER; i++) {
		rdtscll(s);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_SINVACTIVATE, TEST_RESERVE, TEST_COMP, NULL, 0);
		rdtscll(e);
		t1 += (e-s);
		cos_mem_fence();
		if (ret) printc("act i %d ret %d\n", i, ret);
		act[i] = e-s;
		assert(!ret);
		rdtscll(s);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_SINVDEACTIVATE, TEST_RESERVE, lid, 0, 0);
		rdtscll(e);
		t2 += (e-s);
		if (ret) printc("deact i %d ret %d\n", i, ret);
		deact[i] = e-s;
		delay(KERN_QUIESCENCE_CYCLES);
	}
	printc("cap iter %d act %llu deact %llu\n", ITER, t1/ITER, t2/ITER);
	stddev(act, "act");
	stddev(deact, "deact");
}

static void
mem_test(void)
{
	unsigned long long s, e, t1, t2;
	vaddr_t addr, umem;
	pgtblcap_t pgtbl;
	int i, ret, lid, j;

	lid = livenessid_bump_alloc(PMEM_TEST);
	addr = (vaddr_t)cos_page_bump_alloc_ext(&test_info, PMEM_TEST);
	ret  = call_cap_op(TEST_PT, CAPTBL_OP_MEMDEACTIVATE, addr, lid, 0, 0);
	if (ret) printc("mem unmap fail %x ret %d\n", addr, ret);
#if PMEM_TEST == 1
	umem = test_info.pmem_mi.untyped_ptr;
	pgtbl = TEST_PMEM_PT;
#else
	umem = test_info.mi.untyped_ptr;
	pgtbl = TEST_UNTYPED_PT;
#endif
	ret = call_cap_op(pgtbl, CAPTBL_OP_MEM_RETYPE2USER, umem, 0, 0, 0);
	if (ret) printc("retype to user fail ret %d\n", ret);

	t1 = t2 = 0;
	for(i=0; i<ITER; i++) {
		rdtscll(s);
		ret = call_cap_op(pgtbl, CAPTBL_OP_MEMACTIVATE, umem, TEST_PT, addr, 0);
		rdtscll(e);
		t1 += (e-s);
		cos_mem_fence();
		if (ret) printc("mem map %x i %d ret %d\n", addr, i, ret);
		act[i] = e-s;
		assert(!ret);
		rdtscll(s);
		ret = call_cap_op(TEST_PT, CAPTBL_OP_MEMDEACTIVATE, addr, lid, 0, 0);
		rdtscll(e);
		t2 += (e-s);
		if (ret) printc("mem unmap %x i %d ret %d\n", addr, i, ret);
		deact[i] = e-s;
		assert(!ret);
	}
	printc("mem iter %d map %llu unmap %llu\n", ITER, t1/ITER, t2/ITER);
	stddev(act, "map");
	stddev(deact, "unmap");
}

static void
thd_fn_perf(void *d)
{
	cos_thd_switch(TEST_THREAD);
	printc(" I am new thd\n");

	while(1) {
		cos_thd_switch(TEST_THREAD);
	}
	printc("Error, shouldn't get here!\n");
}

static void
thd_switch_test(void)
{
	thdcap_t ts;
	long long total_swt_cycles = 0;
	long long s = 0, e = 0;
	int i;

	ts = cos_thd_alloc_ext(&test_info, TEST_COMP, thd_fn_perf, NULL, PMEM_TEST);
	assert(ts);
	cos_thd_switch(ts);

	for (i = 0 ; i < ITER ; i++) {
		rdtscll(s);
		cos_thd_switch(ts);
		rdtscll(e);
		act[i] = (int)((e-s)/2LL);
		total_swt_cycles += (e-s)/2LL;
	}

	printc("Average THD SWTCH (Total: %lld / Iterations: %lld ): %lld\n",
		total_swt_cycles, (long long) ITER, (total_swt_cycles / (long long)ITER));
	stddev(act, "thread switch");
}

static void
async_thd_fn_perf(void *thdcap)
{
	thdcap_t tc = (thdcap_t)thdcap;
	arcvcap_t rc = rcc_global;
	int i;

	cos_rcv(rc);
	printc("I am arecv\n");

	for (i = 0 ; i < ITER + 1 ; i++) cos_rcv(rc);

	cos_thd_switch(tc);
}

static void
async_thd_parent_perf(void *thdcap)
{
	thdcap_t tc = (thdcap_t)thdcap;
	arcvcap_t rc = rcp_global;
	asndcap_t sc = scp_global;
	long long total_asnd_cycles = 0;
	long long s = 0, e = 0;
	int i;

	cos_asnd(sc);
	printc("I am asnd\n");

	for (i = 0 ; i < ITER ; i++) {
		rdtscll(s);
		cos_asnd(sc);
		rdtscll(e);
		act[i] = (int)(e-s);
		total_asnd_cycles += (e-s);
	}

	printc("Average ASND/ARCV (Total: %lld / Iterations: %lld ): %lld\n",
		total_asnd_cycles, (long long) (ITER), (total_asnd_cycles / (long long)(ITER)));
	stddev(act, "round trip asnd");

	async_test_flag = 0;
	cos_thd_switch(tc);
}

static void
async_snd_test(void)
{
	thdcap_t tcp, tcc;
	tcap_t tccp, tccc;
	arcvcap_t rcp, rcc;

	/* parent rcv capabilities */
	tcp = cos_thd_alloc_ext(&test_info, TEST_COMP, async_thd_parent_perf, (void*)TEST_THREAD, PMEM_TEST);
	assert(tcp);
	tccp = cos_tcap_alloc_ext(&test_info, TCAP_PRIO_MAX + 2, PMEM_TEST);
	assert(tccp);
	rcp = cos_arcv_alloc_ext(&test_info, tcp, tccp, TEST_COMP, TEST_RECV, PMEM_TEST);
	assert(rcp);
	if (cos_tcap_transfer(rcp, TEST_TCAP, TCAP_RES_INF, TCAP_PRIO_MAX + 1)) assert(0);

	/* child rcv capabilities */
	tcc = cos_thd_alloc_ext(&test_info, TEST_COMP, async_thd_fn_perf, (void*)tcp, PMEM_TEST);
	assert(tcc);
	tccc = cos_tcap_alloc_ext(&test_info, TCAP_PRIO_MAX + 1, PMEM_TEST);
	assert(tccc);
	rcc = cos_arcv_alloc_ext(&test_info, tcc, tccc, TEST_COMP, rcp, PMEM_TEST);
	assert(rcc);
	if (cos_tcap_transfer(rcc, TEST_TCAP, TCAP_RES_INF, TCAP_PRIO_MAX)) assert(0);

	/* make the snd channel to the child */
	scp_global = cos_asnd_alloc_ext(&test_info, rcc, TEST_CT, PMEM_TEST);
	assert(scp_global);

	rcc_global = rcc;
	rcp_global = rcp;

	async_test_flag = 1;
	while (async_test_flag) cos_thd_switch(tcp);
}

int
test_serverfn(int a, int b, int c)
{
	return 0xDEADBEEF;
}

static void
ipc_test(void)
{
	compcap_t cc;
	sinvcap_t ic;
	int i;
	long long total_cycles = 0LL;
	long long s = 0LL, e = 0LL;
	unsigned int ret;

	cc = cos_comp_alloc_ext(&test_info, TEST_CT, TEST_PT, (vaddr_t)NULL, PMEM_TEST);
	assert(cc > 0);
	ic = cos_sinv_alloc_ext(&test_info, cc, (vaddr_t)SERVER_FN(test_serverfn), PMEM_TEST);
	assert(ic > 0);
	ret = call_cap_mb(ic, 1, 2, 3);
	assert(ret == 0xDEADBEEF);

	for (i = 0 ; i < ITER ; i++) {
		rdtscll(s);
		call_cap_mb(ic, 1, 2, 3);
		rdtscll(e);
		act[i] = (int)(e-s);
		total_cycles += (e-s);
	}

	printc("Average SINV (Total: %lld / Iterations: %lld ): %lld\n",
		total_cycles, (long long) (ITER), (total_cycles / (long long)(ITER)));
	stddev(act, "round trip ipc");
}

static int
kernel_flush(void)
{
	int r;
	do {
		r = call_cap(CCFLUSH_CAP_TEMP, 0, 0, 0, 0);
	} while (r < 0);
	return r;
}

static void
kernel_flush_test(void)
{
	int i, j, ret, lid;
	captblcap_t ct[KERNEL_PAGE-OFFSET];
	unsigned long long s, e;

	lid = livenessid_bump_alloc(PMEM_TEST);
	for(i=0; i<KERNEL_PAGE-3; i++) {
		ct[i]= cos_captbl_alloc_ext(&test_info, PMEM_TEST);
	}
	ret = kernel_flush();
	printc("begin flush %d\n", ret);

	for(i=0; i<ITER; i++) {
		for(j=0; j<KERNEL_PAGE-OFFSET; j++) {
			ret = call_cap_op(ct[j], CAPTBL_OP_SINVACTIVATE, TEST_RESERVE, TEST_COMP, NULL, 0);
			assert(!ret);
		}
		for(j=0; j<KERNEL_PAGE-OFFSET; j++) {
			ret = call_cap_op(ct[j], CAPTBL_OP_SINVDEACTIVATE, TEST_RESERVE, lid, 0, 0);
			assert(!ret);
		}

		rdtscll(s);
		ret = kernel_flush();
		rdtscll(e);
//printc("ret %d\n", ret);
		assert(ret == KERNEL_PAGE);
		act[i] = (int)(e-s);
	}
	printc("flush page %d\n", KERNEL_PAGE);
	stddev(act, "kernel flush");
}

void
test_start(int node, vaddr_t untype, vaddr_t vas)
{
	printc("I am micro bench test\n");
	cos_meminfo_init(&test_info.mi, untype, MEM_SIZE, TEST_UNTYPED_PT);
	cos_meminfo_init(&test_info.pmem_mi, untype, MEM_SIZE, TEST_PMEM_PT);
	cos_compinfo_init(&test_info, TEST_PT, TEST_CT, TEST_COMP, vas, TEST_FREE, &test_info);

//	cap_act_test();
//	mem_test();
//	thd_switch_test();
//	async_snd_test();
//	ipc_test();
	kernel_flush_test();

//	pgtbl_cons_test();
//	captbl_cons_test();
//	retype_test();
//	captbl_act_test();

	return ;
}

static void
pgtbl_cons_test(void)
{
	unsigned long long s, e, t1, t2;
	vaddr_t addr, cons_addr;
	pgtblcap_t pgtbl;
	int ret, i;

#if PMEM_TEST == 1
	addr = test_info.pmem_mi.untyped_ptr;
	pgtbl = TEST_PMEM_PT;
#else
	addr = test_info.mi.untyped_ptr;
	pgtbl = TEST_UNTYPED_PT;
#endif
	assert(addr == round_up_to_pgd_page(addr));
	cons_addr = test_info.vas_frontier;
	assert(cons_addr == round_up_to_pgd_page(cons_addr));
	/* retype to kernel memory */
	ret = call_cap_op(pgtbl, CAPTBL_OP_MEM_RETYPE2KERN, addr, 0, 0, 0);
	if (ret) printc("retype to kernel fail ret %d\n", ret);
	/* activate the pte */
	ret = call_cap_op(TEST_CT, CAPTBL_OP_PGTBLACTIVATE, TEST_RESERVE, pgtbl, addr, 1);
	if (ret) printc("activate pte fail ret %d\n", ret);

	/* pgtbl cons/decons */
	t1 = t2 = 0;
	for(i=0; i<ITER; i++) {
		rdtscll(s);
		ret = call_cap_op(TEST_PT, CAPTBL_OP_CONS, TEST_RESERVE, cons_addr, 0, 0);
		rdtscll(e);
		t1 += (e-s);
		if (ret) printc("cons %x i %d ret %d\n", cons_addr, i, ret);
		act[i] = e-s;
		assert(!ret);
		rdtscll(s);
		ret = call_cap_op(TEST_PT, CAPTBL_OP_DECONS, TEST_RESERVE, cons_addr, 1, 0);
		rdtscll(e);
		t2 += (e-s);
		if (ret) printc("decons %x i %d ret %d\n", cons_addr, i, ret);
		deact[i] = e-s;
		assert(!ret);
	}
	printc("pgtbl iter %d cons %llu decons %llu\n", ITER, t1/ITER, t2/ITER);
	stddev(act, "pgtbl cons");
	stddev(deact, "pgtbl decons");
}

static void
captbl_cons_test(void)
{
	unsigned long long s, e, t1, t2;
	vaddr_t addr;
	pgtblcap_t pgtbl;
	int ret, i;

#if PMEM_TEST == 1
	addr = test_info.pmem_mi.untyped_ptr;
	pgtbl = TEST_PMEM_PT;
#else
	addr = test_info.mi.untyped_ptr;
	pgtbl = TEST_UNTYPED_PT;
#endif
	assert(addr == round_up_to_pgd_page(addr));

	ret = call_cap_op(pgtbl, CAPTBL_OP_MEM_RETYPE2KERN, addr, 0, 0, 0);
	if (ret) printc("retype to kernel fail ret %d\n", ret);
	ret = call_cap_op(TEST_CT, CAPTBL_OP_CAPTBLACTIVATE, TEST_RESERVE, pgtbl, addr, 1);
	if (ret) printc("activate captbl fail ret %d\n", ret);

	/* captbl cons/decons */
#define CONS_TEST_CAP (PAGE_SIZE/2/CACHE_LINE*4)
	t1 = t2 = 0;
	for(i=0; i<ITER; i++) {
		rdtscll(s);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_CONS, TEST_RESERVE, CONS_TEST_CAP, 0, 0);
		rdtscll(e);
		t1 += (e-s);
		if (ret) printc("cons %d i %d ret %d\n", CONS_TEST_CAP, i, ret);
		assert(!ret);
		rdtscll(s);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_DECONS, TEST_RESERVE, CONS_TEST_CAP, 1, 0);
		rdtscll(e);
		t2 += (e-s);
		if (ret) printc("decons %d i %d ret %d\n", CONS_TEST_CAP, i, ret);
		assert(!ret);
	}
	printc("captbl iter %d cons %llu decons %llu\n", ITER, t1/ITER, t2/ITER);
}

static void
retype_test(void)
{
	unsigned long long s, e, t1, t2;
	vaddr_t addr;
	pgtblcap_t pgtbl;
	int ret, i;

#if PMEM_TEST == 1
	addr = test_info.pmem_mi.untyped_ptr;
	pgtbl = TEST_PMEM_PT;
#else
	addr = test_info.mi.untyped_ptr;
	pgtbl = TEST_UNTYPED_PT;
#endif
	assert(addr == round_up_to_pgd_page(addr));

	t1 = t2 = 0;
	for(i=0; i<ITER; i++) {
		rdtscll(s);
		ret = call_cap_op(pgtbl, CAPTBL_OP_MEM_RETYPE2USER, addr, 0, 0, 0);
		rdtscll(e);
		t1 += (e-s);
		if (ret) printc("retype user %x i %d ret %d\n", addr, i, ret);
		assert(!ret);
		rdtscll(s);
		ret = call_cap_op(pgtbl, CAPTBL_OP_MEM_RETYPE2FRAME, addr, 0, 0, 0);
		rdtscll(e);
		t2 += (e-s);
		if (ret) printc("retype frame %x i %d ret %d\n", addr, i, ret);
		assert(!ret);
	}
	printc("retype iter %d user %llu frame %llu\n", ITER, t1/ITER, t2/ITER);
}

static void
captbl_act_test(void)
{
	unsigned long long s, e, t1, t2;
	vaddr_t addr;
	pgtblcap_t pgtbl;
	int i, ret, lid;

#if PMEM_TEST == 1
	addr = test_info.pmem_mi.untyped_ptr;
	pgtbl = TEST_PMEM_PT;
#else
	addr = test_info.mi.untyped_ptr;
	pgtbl = TEST_UNTYPED_PT;
#endif
	assert(addr == round_up_to_pgd_page(addr));
	ret = call_cap_op(pgtbl, CAPTBL_OP_MEM_RETYPE2KERN, addr, 0, 0, 0);
	if (ret) printc("retype to kernel fail ret %d\n", ret);
	lid = livenessid_bump_alloc(PMEM_TEST);

	t1 = t2 = 0;
	for(i=0; i<ITER; i++) {
		rdtscll(s);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_CAPTBLACTIVATE, TEST_RESERVE, pgtbl, addr, 1);
		rdtscll(e);
		t1 += (e-s);
		if (ret) printc("captbl act %x i %d ret %d\n", addr, i, ret);
		assert(!ret);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_CAPKMEM_FREEZE, TEST_RESERVE, 0, 0, 0);
		rdtscll(s);
		ret = call_cap_op(TEST_CT, CAPTBL_OP_CAPTBLDEACTIVATE_ROOT, TEST_RESERVE, lid, pgtbl, addr);
		rdtscll(e);
		t2 += (e-s);
		if (ret) printc("captbl deact %x i %d ret %d\n", addr, i, ret);
		assert(!ret);
	}
	printc("captbl iter %d act %llu deact %llu\n", ITER, t1/ITER, t2/ITER);
}
