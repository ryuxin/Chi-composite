#include "micro_booter.h"

struct meta_header {
	char magic[MAGIC_LEN];
	int kernel_done, boot_done, node_num, boot_num;
};
struct meta_header *ivshmem_meta;
int cur_node;
static void
thd_fn_perf(void *d)
{
	cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE);

	while(1) {
		cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE);
	}
	PRINTC("Error, shouldn't get here!\n");
}

static void
test_thds_perf(void)
{
	thdcap_t ts;
	long long total_swt_cycles = 0;
	long long start_swt_cycles = 0, end_swt_cycles = 0;
	int i;

	ts = cos_thd_alloc(&booter_info, booter_info.comp_cap, thd_fn_perf, NULL);
	assert(ts);
	cos_thd_switch(ts);

	rdtscll(start_swt_cycles);
	for (i = 0 ; i < ITER ; i++) {
		cos_thd_switch(ts);
	}
	rdtscll(end_swt_cycles);
	total_swt_cycles = (end_swt_cycles - start_swt_cycles) / 2LL;

	PRINTC("Average THD SWTCH (Total: %lld / Iterations: %lld ): %lld\n",
		total_swt_cycles, (long long) ITER, (total_swt_cycles / (long long)ITER));
}

static void
thd_fn(void *d)
{
	PRINTC("\tNew thread %d with argument %d, capid %ld\n", cos_thdid(), (int)d, tls_test[(int)d]);
	/* Test the TLS support! */
	assert(tls_get(0) == tls_test[(int)d]);
	while (1) cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE);
	PRINTC("Error, shouldn't get here!\n");
}

static void
test_thds(void)
{
	thdcap_t ts[TEST_NTHDS];
	int i;

	for (i = 0 ; i < TEST_NTHDS ; i++) {
		ts[i] = cos_thd_alloc(&booter_info, booter_info.comp_cap, thd_fn, (void *)i);
		assert(ts[i]);
		tls_test[i] = i;
		cos_thd_mod(&booter_info, ts[i], &tls_test[i]);
		PRINTC("switchto %d @ %x\n", (int)ts[i], cos_introspect(&booter_info, ts[i], THD_GET_IP));
		cos_thd_switch(ts[i]);
	}

	PRINTC("test done\n");
}

#define TEST_NPAGES (1024*2) 	/* Testing with 8MB for now */

static void
test_mem(void)
{
	char *p, *s, *t, *prev;
	int i;
	const char *chk = "SUCCESS";

	p = cos_page_bump_alloc(&booter_info);
	assert(p);
	strcpy(p, chk);

	assert(0 == strcmp(chk, p));
	PRINTC("%s: Page allocation\n", p);

	s = cos_page_bump_alloc(&booter_info);
	assert(s);
	prev = s;
	for (i = 0 ; i < TEST_NPAGES ; i++) {
		t = cos_page_bump_alloc(&booter_info);
		assert(t && t == prev + 4096);
		prev = t;
	}
	memset(s, 0, TEST_NPAGES * 4096);
	PRINTC("SUCCESS: Allocated and zeroed %d pages.\n", TEST_NPAGES);
}

volatile arcvcap_t rcc_global, rcp_global;
volatile asndcap_t scp_global;
int async_test_flag = 0;

static void
async_thd_fn_perf(void *thdcap)
{
	thdcap_t tc = (thdcap_t)thdcap;
	arcvcap_t rc = rcc_global;
	int i;

	cos_rcv(rc);

	for (i = 0 ; i < ITER + 1 ; i++) {
		cos_rcv(rc);
	}

	cos_thd_switch(tc);
}

static void
async_thd_parent_perf(void *thdcap)
{
	thdcap_t tc = (thdcap_t)thdcap;
	arcvcap_t rc = rcp_global;
	asndcap_t sc = scp_global;
	long long total_asnd_cycles = 0;
	long long start_asnd_cycles = 0, end_arcv_cycles = 0;
	int i;

	cos_asnd(sc);

	rdtscll(start_asnd_cycles);
	for (i = 0 ; i < ITER ; i++) {
		cos_asnd(sc);
	}
	rdtscll(end_arcv_cycles);
	total_asnd_cycles = (end_arcv_cycles - start_asnd_cycles) / 2;

	PRINTC("Average ASND/ARCV (Total: %lld / Iterations: %lld ): %lld\n",
		total_asnd_cycles, (long long) (ITER), (total_asnd_cycles / (long long)(ITER)));

	async_test_flag = 0;
	cos_thd_switch(tc);
}

static void
async_thd_fn(void *thdcap)
{
	thdcap_t tc = (thdcap_t)thdcap;
	arcvcap_t rc = rcc_global;
	thdid_t tid;
	int rcving;
	cycles_t cycles;
	int pending;

	PRINTC("Asynchronous event thread handler.\n");
	PRINTC("<-- rcving...\n");
	pending = cos_rcv(rc);
	PRINTC("<-- pending %d\n", pending);
	PRINTC("<-- rcving...\n");
	pending = cos_rcv(rc);
	PRINTC("<-- pending %d\n", pending);
	PRINTC("<-- rcving...\n");
	pending = cos_rcv(rc);
	PRINTC("<-- Error: manually returning to snding thread.\n");
	cos_thd_switch(tc);
	PRINTC("ERROR: in async thd *after* switching back to the snder.\n");
	while (1) ;
}

static void
async_thd_parent(void *thdcap)
{
	thdcap_t  tc = (thdcap_t)thdcap;
	arcvcap_t rc = rcp_global;
	asndcap_t sc = scp_global;
	int ret, pending;
	thdid_t tid;
	int rcving;
	cycles_t cycles;

	PRINTC("--> sending\n");
	ret = cos_asnd(sc);
	if (ret) PRINTC("asnd returned %d.\n", ret);
	PRINTC("--> Back in the asnder.\n");
	PRINTC("--> sending\n");
	ret = cos_asnd(sc);
	if (ret) PRINTC("--> asnd returned %d.\n", ret);
	PRINTC("--> Back in the asnder.\n");
	PRINTC("--> receiving to get notifications\n");
	pending = cos_sched_rcv(rc, &tid, &rcving, &cycles);
	PRINTC("--> pending %d, thdid %d, rcving %d, cycles %lld\n", pending, tid, rcving, cycles);

	async_test_flag = 0;
	cos_thd_switch(tc);
}

static void
test_async_endpoints(void)
{
	thdcap_t  tcp,  tcc;
	tcap_t    tccp, tccc;
	arcvcap_t rcp,  rcc;
	int ret;

	PRINTC("Creating threads, and async end-points.\n");
	/* parent rcv capabilities */
	tcp = cos_thd_alloc(&booter_info, booter_info.comp_cap, async_thd_parent, (void*)BOOT_CAPTBL_SELF_INITTHD_BASE);
	assert(tcp);
	tccp = cos_tcap_alloc(&booter_info, TCAP_PRIO_MAX + 2);
	assert(tccp);
	rcp = cos_arcv_alloc(&booter_info, tcp, tccp, booter_info.comp_cap, BOOT_CAPTBL_SELF_INITRCV_BASE);
	assert(rcp);
	if ((ret = cos_tcap_transfer(rcp, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_RES_INF, TCAP_PRIO_MAX + 1))) {
		PRINTC("transfer failed: %d\n", ret);
		assert(0);
	}

	/* child rcv capabilities */
	tcc = cos_thd_alloc(&booter_info, booter_info.comp_cap, async_thd_fn, (void*)tcp);
	assert(tcc);
	tccc = cos_tcap_alloc(&booter_info, TCAP_PRIO_MAX + 1);
	assert(tccc);
	rcc = cos_arcv_alloc(&booter_info, tcc, tccc, booter_info.comp_cap, rcp);
	assert(rcc);
	if (cos_tcap_transfer(rcc, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_RES_INF, TCAP_PRIO_MAX)) assert(0);

	/* make the snd channel to the child */
	scp_global = cos_asnd_alloc(&booter_info, rcc, booter_info.captbl_cap);
	assert(scp_global);

	rcc_global = rcc;
	rcp_global = rcp;

	async_test_flag = 1;
	while (async_test_flag) cos_thd_switch(tcp);

	PRINTC("Async end-point test successful.\n");
	PRINTC("Test done.\n");
}

static void
test_async_endpoints_perf(void)
{
	thdcap_t tcp, tcc;
	tcap_t tccp, tccc;
	arcvcap_t rcp, rcc;

	/* parent rcv capabilities */
	tcp = cos_thd_alloc(&booter_info, booter_info.comp_cap, async_thd_parent_perf, (void*)BOOT_CAPTBL_SELF_INITTHD_BASE);
	assert(tcp);
	tccp = cos_tcap_alloc(&booter_info, TCAP_PRIO_MAX + 2);
	assert(tccp);
	rcp = cos_arcv_alloc(&booter_info, tcp, tccp, booter_info.comp_cap, BOOT_CAPTBL_SELF_INITRCV_BASE);
	assert(rcp);
	if (cos_tcap_transfer(rcp, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_RES_INF, TCAP_PRIO_MAX + 1)) assert(0);

	/* child rcv capabilities */
	tcc = cos_thd_alloc(&booter_info, booter_info.comp_cap, async_thd_fn_perf, (void*)tcp);
	assert(tcc);
	tccc = cos_tcap_alloc(&booter_info, TCAP_PRIO_MAX + 1);
	assert(tccc);
	rcc = cos_arcv_alloc(&booter_info, tcc, tccc, booter_info.comp_cap, rcp);
	assert(rcc);
	if (cos_tcap_transfer(rcc, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_RES_INF, TCAP_PRIO_MAX)) assert(0);

	/* make the snd channel to the child */
	scp_global = cos_asnd_alloc(&booter_info, rcc, booter_info.captbl_cap);
	assert(scp_global);

	rcc_global = rcc;
	rcp_global = rcp;

	async_test_flag = 1;
	while (async_test_flag) cos_thd_switch(tcp);
}

#define TCAP_NLAYERS 3
static volatile int child_activated[TCAP_NLAYERS][2];
/* tcap child/parent receive capabilities, and the send capability */
static volatile arcvcap_t tc_crc[TCAP_NLAYERS][2], tc_prc[TCAP_NLAYERS][2];
static volatile asndcap_t tc_sc[TCAP_NLAYERS][3];

static void
tcap_child(void *d)
{
	arcvcap_t __tc_crc = (arcvcap_t)d;

	while (1) {
		int pending;

		pending = cos_rcv(__tc_crc);
		PRINTC("tcap_test:rcv: pending %d\n", pending);
	}
}

static void
tcap_parent(void *d)
{
	int i;
	asndcap_t __tc_sc = (asndcap_t)d;

	for (i = 0 ; i < ITER ; i++) {
		cos_asnd(__tc_sc);
	}
}

/* static void */
/* test_tcaps(void) */
/* { */
/* 	thdcap_t tcp, tcc; */
/* 	tcap_t tccp, tccc; */
/* 	arcvcap_t rcp, rcc; */

/* 	/\* parent rcv capabilities *\/ */
/* 	tcp = cos_thd_alloc(&booter_info, booter_info.comp_cap, tcap_parent, (void*)BOOT_CAPTBL_SELF_INITTHD_BASE); */
/* 	assert(tcp); */
/* 	tccp = cos_tcap_split(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, 0, 0); */
/* 	assert(tccp); */
/* 	rcp = cos_arcv_alloc(&booter_info, tcp, tccp, booter_info.comp_cap, BOOT_CAPTBL_SELF_INITRCV_BASE); */
/* 	assert(rcp); */

/* 	/\* child rcv capabilities *\/ */
/* 	tcc = cos_thd_alloc(&booter_info, booter_info.comp_cap, tcap_child, (void*)tcp); */
/* 	assert(tcc); */
/* 	tccc = cos_tcap_split(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, 0, 0); */
/* 	assert(tccc); */
/* 	rcc = cos_arcv_alloc(&booter_info, tcc, tccc, booter_info.comp_cap, rcp); */
/* 	assert(rcc); */

/* 	/\* make the snd channel to the child *\/ */
/* 	scp_global = cos_asnd_alloc(&booter_info, rcc, booter_info.captbl_cap); */
/* 	assert(scp_global); */

/* 	rcc_global = rcc; */
/* 	rcp_global = rcp; */

/* 	async_test_flag = 1; */
/* 	while (async_test_flag) cos_thd_switch(tcp); */
/* } */

static void
spinner(void *d)
{ while (1) ; }

static void
test_timer(void)
{
	int i;
	thdcap_t tc;
	cycles_t c = 0, p = 0, t = 0;

	PRINTC("Starting timer test.\n");
	tc = cos_thd_alloc(&booter_info, booter_info.comp_cap, spinner, NULL);

	for (i = 0 ; i <= 16 ; i++) {
		thdid_t tid;
		int rcving;
		cycles_t cycles;

		cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_BASE, &tid, &rcving, &cycles);
		cos_thd_switch(tc);
		p = c;
		rdtscll(c);
		if (i > 0) t += c-p;
	}

	PRINTC("\tCycles per tick (10 microseconds) = %lld\n", t/16);

	PRINTC("Timer test completed.\n");
	PRINTC("Success.\n");
}

long long midinv_cycles = 0LL;

int
test_serverfn(int a, int b, int c)
{
	rdtscll(midinv_cycles);
	return 0xDEADBEEF;
}

extern void *__inv_test_serverfn(int a, int b, int c);

static inline
int call_cap_mb(u32_t cap_no, int arg1, int arg2, int arg3)
{
	int ret;

	/*
	 * Which stack should we use for this invocation?  Simple, use
	 * this stack, at the current sp.  This is essentially a
	 * function call into another component, with odd calling
	 * conventions.
	 */
	cap_no = (cap_no + 1) << COS_CAPABILITY_OFFSET;

	__asm__ __volatile__( \
		"pushl %%ebp\n\t" \
		"movl %%esp, %%ebp\n\t" \
		"movl %%esp, %%edx\n\t" \
		"movl $1f, %%ecx\n\t" \
		"sysenter\n\t" \
		"1:\n\t" \
		"popl %%ebp" \
		: "=a" (ret)
		: "a" (cap_no), "b" (arg1), "S" (arg2), "D" (arg3) \
		: "memory", "cc", "ecx", "edx");

	return ret;
}

static void
test_inv(void)
{
	compcap_t cc;
	sinvcap_t ic;
	unsigned int r;

	cc = cos_comp_alloc(&booter_info, booter_info.captbl_cap, booter_info.pgtbl_cap, (vaddr_t)NULL);
	assert(cc > 0);
	ic = cos_sinv_alloc(&booter_info, cc, (vaddr_t)__inv_test_serverfn);
	assert(ic > 0);

	r = call_cap_mb(ic, 1, 2, 3);
	PRINTC("Return from invocation: %x (== DEADBEEF?)\n", r);
	PRINTC("Test done.\n");
}

static void
test_inv_perf(void)
{
	compcap_t cc;
	sinvcap_t ic;
	int i;
	long long total_cycles = 0LL;
	long long total_inv_cycles = 0LL, total_ret_cycles = 0LL;
	unsigned int ret;

	cc = cos_comp_alloc(&booter_info, booter_info.captbl_cap, booter_info.pgtbl_cap, (vaddr_t)NULL);
	assert(cc > 0);
	ic = cos_sinv_alloc(&booter_info, cc, (vaddr_t)__inv_test_serverfn);
	assert(ic > 0);
	ret = call_cap_mb(ic, 1, 2, 3);
	assert(ret == 0xDEADBEEF);

	for (i = 0 ; i < ITER ; i++) {
		long long start_cycles = 0LL, end_cycles = 0LL;

		midinv_cycles = 0LL;
		rdtscll(start_cycles);
		call_cap_mb(ic, 1, 2, 3);
		rdtscll(end_cycles);
		total_inv_cycles += (midinv_cycles - start_cycles);
		total_ret_cycles += (end_cycles - midinv_cycles);
	}

	PRINTC("Average SINV (Total: %lld / Iterations: %lld ): %lld\n",
		total_inv_cycles, (long long) (ITER), (total_inv_cycles / (long long)(ITER)));
	PRINTC("Average SRET (Total: %lld / Iterations: %lld ): %lld\n",
		total_ret_cycles, (long long) (ITER), (total_ret_cycles / (long long)(ITER)));

	long long tstart_cycles = 0LL, tend_cycles = 0LL;
	rdtscll(tstart_cycles);
	for (i = 0 ; i < ITER ; i++) {
		call_cap_mb(ic, 1, 2, 3);
	}
	rdtscll(tend_cycles);

	PRINTC("Average SINV (Total: %lld / Iterations: %lld ): %lld\n",
		tend_cycles-tstart_cycles, (long long) (ITER), ((tend_cycles-tstart_cycles) / (long long)(ITER)));
}

void
test_captbl_expand(void)
{
	int i;
	compcap_t cc;

	cc = cos_comp_alloc(&booter_info, booter_info.captbl_cap, booter_info.pgtbl_cap, (vaddr_t)NULL);
	assert(cc);
	for (i = 0 ; i < 1024 ; i++) {
		sinvcap_t ic;

		ic = cos_sinv_alloc(&booter_info, cc, (vaddr_t)__inv_test_serverfn);
		assert(ic > 0);
	}
	PRINTC("Captbl expand SUCCESS.\n");
}

void
test_run(void)
{
	timer_attach();
	test_timer();
	timer_detach();

	/* 
	 * It is ideal to ubenchmark kernel API with timer interrupt detached,
	 * Not so much for unit-tests
	 */
	test_thds();
	test_thds_perf();

	test_mem();

	test_async_endpoints();
	test_async_endpoints_perf();

	test_inv();
	test_inv_perf();

	test_captbl_expand();
}

