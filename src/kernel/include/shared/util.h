#ifndef UTIL_H
#define UTIL_H

#define CAS_SUCCESS 1
#define ENABLE_CLFLUSHOPT 1

/* 
 * Return values:
 * 0 on failure due to contention (*target != old)
 * 1 otherwise (*target == old -> *target = updated)
 */
static inline int 
cos_cas(unsigned long *target, unsigned long old, unsigned long updated)
{
	char z;
	__asm__ __volatile__("lock cmpxchgl %2, %0; setz %1"
			     : "+m" (*target),
			       "=a" (z)
			     : "q"  (updated),
			       "a"  (old)
			     : "memory", "cc");
	return (int)z;
}

/* Fetch-and-add implementation on x86. It returns the original value
 * before xaddl. */
static inline int 
cos_faa(int *var, int value)
{
	asm volatile("lock xaddl %%eax, %2;"
		     :"=a" (value)            //Output
		     :"a" (value), "m" (*var) //Input
		     :"memory");
	return value;
}

/* x86 cpuid instruction barrier. */
static inline void
cos_inst_bar(void)
{
	int eax, edx, code = 0;
	
	asm volatile("cpuid"
		     :"=a"(eax),"=d"(edx)
		     :"a"(code)
		     :"ecx","ebx");

	return;
}

static inline void
cos_flush_cache(void *p)
{
//return ;
#ifdef ENABLE_CLFLUSHOPT
	__asm__ __volatile__("clflushopt (%0)" :: "r"(p));
#else
	__asm__ __volatile__("clflush (%0)" :: "r"(p));
#endif
}

static inline void
cos_wb_cache(void *p)
{
//return ;
#ifdef ENABLE_CLFLUSHOPT
	__asm__ __volatile__("clflushopt (%0)" :: "r"(p));
#else
	__asm__ __volatile__("clflush (%0)" :: "r"(p));
#endif
}

#ifndef rdtscll
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))
#endif

#endif
