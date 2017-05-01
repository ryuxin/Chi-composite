#include "micro_booter.h"
#include "mem_mgr.h"

#define CVECT_ALLOC() alloc_pages(1)
#define CVECT_FREE(x) free_pages(x, 1)
#include <cmap.h>
#define CSLAB_ALLOC(sz)   alloc_pages(1)
#define CSLAB_FREE(x, sz) free_pages(x, 1)
#include <cslab.h>

static struct cos_compinfo men_mgr_info, client_info[NUM_NODE];
CMAP_CREATE_STATIC(mem_objs);
CSLAB_CREATE(metas, sizeof(struct mem_meta));

void
mem_mgr_init(vaddr_t untype, int size, vaddr_t vas_start)
{
	int i;

	/* two mem_mgr may manages a same client, make sure they use different portion of client's virtual address space */
	cos_meminfo_init(&men_mgr_info.mi, untype, size, MEM_SELF_PMEM_PT);
	cos_meminfo_init(&men_mgr_info.pmem_mi, untype, size, MEM_SELF_PMEM_PT);
	cos_compinfo_init(&men_mgr_info, MEM_SELF_PT, MEM_SELF_CT, 0, (vaddr_t)cos_get_heap_ptr()+PAGE_SIZE, MEM_CAPTBL_FREE, &men_mgr_info);
	for(i=0; i<NUM_NODE; i++) {
		cos_compinfo_init(&client_info[i], MEM_COMP_PT_BASE+i*CAP32B_IDSZ, 0, 0, vas_start, 0, &men_mgr_info);
	}
}

void
free_pages(void *addr, int n)
{
	return ;
}

void *
alloc_pages(int n)
{
	int i;
	vaddr_t dst, start;
	
	start = (vaddr_t)cos_page_bump_alloc(&men_mgr_info);
	for(i=1; i<n; i++) {
		dst = (vaddr_t)cos_page_bump_alloc(&men_mgr_info);
		assert(dst == start + i*PAGE_SIZE);
	}
	return (void *)start;
}

void *
alias_pages(int node, void *addr, int size)
{
	int i;
	vaddr_t dst, start;

	start = cos_mem_alias(&client_info[node], &men_mgr_info, (vaddr_t)addr);
	for(i=1; i<size; i++) {
		dst = cos_mem_alias(&client_info[node], &men_mgr_info, (vaddr_t)addr+i*PAGE_SIZE);
		assert(dst == start + i*PAGE_SIZE);
	}
	return (void *)start;
}

int
mem_create(void *addr, int size)
{
	struct mem_meta *meta;
	int id;

	meta = cslab_alloc_metas();
	meta->size   = size;
	meta->refcnt = 0;
	meta->addr   = (vaddr_t)addr;
	id = cmap_add(&mem_objs, meta);
	memset(meta->dest, 0, sizeof(meta->dest));

	return id;
}

void *
mem_retrieve(int memid, int node)
{
	struct mem_meta *meta;

	meta = (struct mem_meta *)cmap_lookup(&mem_objs, memid);
	if (!meta->dest[node]) {
		meta->dest[node] = (vaddr_t)alias_pages(node, (void *)meta->addr, meta->size/PAGE_SIZE);
		meta->refcnt++;
	}

	return (void *)meta->dest[node];
}

struct mem_meta *
mem_lookup(int memid)
{
	return (struct mem_meta *)cmap_lookup(&mem_objs, memid);
}
