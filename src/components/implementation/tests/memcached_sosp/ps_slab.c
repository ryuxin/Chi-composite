/***
 * Copyright 2011-2015 by Gabriel Parmer.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2011
 *
 * History:
 * - Initial slab allocator, 2011
 * - Adapted for parsec, 2015
 */

#include "ps_slab.h"

/* 
 * Default allocation and deallocation functions: assume header is
 * internal to the slab's memory
 */
struct ps_slab *
ps_slab_defalloc(struct ps_mem *m, size_t sz, coreid_t coreid)
{ 
	struct ps_slab *s = ps_plat_alloc(sz, coreid);
	(void)coreid; (void)m;

	if (!s) return NULL;
	s->memory = s;
	return s;
}

void
ps_slab_deffree(struct ps_mem *m, struct ps_slab *s, size_t sz, coreid_t coreid)
{ (void)m; ps_plat_free(s, sz, coreid); }

void
__ps_slab_init(struct ps_slab *s, struct ps_slab_info *si, PS_SLAB_PARAMS)
{
	size_t nfree, i;
	size_t objmemsz  = __ps_slab_objmemsz(obj_sz);
	struct ps_mheader *alloc, *prev;
	PS_SLAB_DEWARN;

	s->nfree    = nfree = (allocsz - headoff) / objmemsz;
	s->memsz    = allocsz;
	s->coreid   = coreid;

	/*
	 * Set up the slab's freelist
	 *
	 * TODO: cache coloring
	 */
	alloc = (struct ps_mheader *)((char *)s->memory + headoff);
	prev  = s->freelist = alloc;
	for (i = 0 ; i < nfree ; i++, prev = alloc, alloc = (struct ps_mheader *)((char *)alloc + objmemsz)) {
		__ps_mhead_init(alloc, s);
		prev->next = alloc;
	}
	/* better not overrun memory */
	assert((void *)alloc <= (void *)((char*)s->memory + allocsz));

	ps_list_init(s, list);
	__slab_freelist_add(&si->fl, s);
	__ps_slab_freelist_check(&si->fl);
}

void
ps_slabptr_init(struct ps_mem *m)
{
	int i;

	memset(m, 0, sizeof(struct ps_mem));

	for (i = 0 ; i < PS_NUMCORES ; i++) {
		m->percore[i].slab_info.fl.list = NULL;
		m->percore[i].slab_info.el.list = NULL;
	}
}

int
ps_slabptr_isempty(struct ps_mem *m)
{
	int i, j;

	for (i = 0 ; i < PS_NUMCORES ; i++) {
		if (m->percore[i].slab_info.nslabs) return 0;
	}
	return 1;
}

