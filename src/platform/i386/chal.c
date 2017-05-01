#include <chal.h>
#include <shared/cos_types.h>
#include "kernel.h"
#include "mem_layout.h"
#include "ivshmem.h"

u32_t free_thd_id = 1;
char timer_detector[PAGE_SIZE] PAGE_ALIGNED;
extern void *cos_kmem, *cos_kmem_base;

paddr_t chal_kernel_mem_pa;

void *
chal_pa2va(paddr_t address)
{ 
	if (ivshmem_addr) {
		if (PA_IN_IVSHMEM_RANGE(address)) {
			return (void *)(address-ivshmem_phy_addr+ivshmem_addr);
		}
	}
	return (void*)(address+COS_MEM_KERN_START_VA); 
}

paddr_t
chal_va2pa(void *address)
{ 
	if (ivshmem_addr) {
		if (VA_IN_IVSHMEM_RANGE(address)) {
			return (paddr_t)(address-ivshmem_addr+ivshmem_phy_addr);
		}
	}
	return (paddr_t)(address-COS_MEM_KERN_START_VA); 
}

void *
chal_alloc_kern_mem(int order)
{ return mem_kmem_start(); }

void chal_free_kern_mem(void *mem, int order) {}

int
chal_attempt_arcv(struct cap_arcv *arcv)
{ return 0; }

void chal_send_ipi(int cpuid) {}

void
chal_khalt(void)
{ khalt(); }

void
chal_init(void)
{ chal_kernel_mem_pa = chal_va2pa(mem_kmem_start()); }
