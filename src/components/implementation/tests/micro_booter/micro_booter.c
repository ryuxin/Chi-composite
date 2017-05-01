#include "micro_booter.h"

struct cos_compinfo booter_info;
thdcap_t termthd; 		/* switch to this to shutdown */
unsigned long tls_test[TEST_NTHDS];
char flush[NUM_LINE*CACHE_LINE] CACHE_ALIGNED;

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

void
timer_attach(void)
{
	cos_hw_attach(BOOT_CAPTBL_SELF_INITHW_BASE, HW_PERIODIC, BOOT_CAPTBL_SELF_INITRCV_BASE);
	PRINTC("\t%d cycles per microsecond\n", cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE));
}

void
timer_detach(void)
{ cos_hw_detach(BOOT_CAPTBL_SELF_INITHW_BASE, HW_PERIODIC); }

static void
test_clflush_perf(void)
{
	unsigned long long s, e, read, modify, invalid;
	int i, j;
	char *ptr, c;

	read = modify = invalid = 0;
	for(i=0; i<ITER; i++) {
		//read cache line
		for(j=0; j<SIZE; j++) c = flush[j];
		rdtscll(s);
		for(ptr = flush; ptr<&flush[SIZE-1]; ptr += CACHE_LINE) cos_flush_cache(ptr);
		rdtscll(e);
		read += (e-s);
		//modify cache line
		for(j=0; j<SIZE; j++) flush[j] = '$';
		rdtscll(s);
		for(ptr = flush; ptr<&flush[SIZE-1]; ptr += CACHE_LINE) cos_flush_cache(ptr);
		rdtscll(e);
		modify += (e-s);
		//invalid cache line
		rdtscll(s);
		for(ptr = flush; ptr<&flush[SIZE-1]; ptr += CACHE_LINE) cos_flush_cache(ptr);
		rdtscll(e);
		invalid += (e-s);
	}
	printc("tot itertion %d avg flush one line read %llu modify %llu invalid %llu\n", NUM_LINE*ITER, read/(NUM_LINE*ITER), modify/(NUM_LINE*ITER), invalid/(NUM_LINE*ITER));
}

void
cos_init(void)
{
	cos_meminfo_init(&booter_info.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_compinfo_init(&booter_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			  (vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, &booter_info);

	termthd = cos_thd_alloc(&booter_info, booter_info.comp_cap, term_fn, NULL);
	assert(termthd);

	PRINTC("\nMicro Booter started.\n");
//	test_clflush_perf();
	test_run();
	PRINTC("\nMicro Booter done.\n");

	cos_thd_switch(termthd);

	return;
}
