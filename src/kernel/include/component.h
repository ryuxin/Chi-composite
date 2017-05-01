/**
 * Copyright 2014 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#ifndef COMPONENT_H
#define COMPONENT_H

#include "liveness_tbl.h"
#include "captbl.h"
#include "pgtbl.h"
#include "cap_ops.h"

struct comp_info {
	struct liveness_data liveness;
	pgtbl_t pgtbl;
	struct captbl *captbl;
	struct cos_sched_data_area *comp_nfo;
} __attribute__((packed));

struct cap_comp {
	struct cap_header h;
	vaddr_t entry_addr;
	struct cap_pgtbl *pgd;
	struct cap_captbl *ct_top;
	struct comp_info info;
} __attribute__((packed));

static int 
comp_activate(struct captbl *t, capid_t cap, capid_t capin, capid_t captbl_cap, capid_t pgtbl_cap, 
	      livenessid_t lid, vaddr_t entry_addr, struct cos_sched_data_area *sa)
{
	struct cap_comp   *compc;
	struct cap_pgtbl  *ptc;
	struct cap_captbl *ctc;
	u32_t v;
	int ret = 0, pmem_ct_cap, pmem_pt_cap, pmem_mem;

	ctc = (struct cap_captbl *)captbl_lkup(t, captbl_cap);
	if (unlikely(!ctc || ctc->h.type != CAP_CAPTBL || ctc->lvl > 0)) return -EINVAL;
	ptc = (struct cap_pgtbl *)captbl_lkup(t, pgtbl_cap);
	if (unlikely(!ptc || ptc->h.type != CAP_PGTBL || ptc->lvl > 0)) return -EINVAL;

	/* global component has reference to global cap_tbl and pg_tbl*/
	pmem_ct_cap = VA_IN_IVSHMEM_RANGE(ctc);
	pmem_pt_cap = VA_IN_IVSHMEM_RANGE(ptc);
	pmem_mem = VA_IN_IVSHMEM_RANGE(ctc->captbl);
	if (pmem_mem) {
		assert(PA_IN_IVSHMEM_RANGE(ptc->pgtbl));
		assert(lid >= LTBL_ENTS);
	}

	v = ptc->refcnt_flags;
	if (v & CAP_MEM_FROZEN_FLAG) return -EINVAL;
	if (pmem_pt_cap) {
		if (cos_non_cc_cas((unsigned long *)&ptc->refcnt_flags, v, v + 1) != CAS_SUCCESS) return -ECASFAIL;
	} else {
		if (cos_cas((unsigned long *)&ptc->refcnt_flags, v, v + 1) != CAS_SUCCESS) return -ECASFAIL;
	}

	v = ctc->refcnt_flags;
	if (v & CAP_MEM_FROZEN_FLAG) cos_throw(undo_ptc, -EINVAL);
	if (pmem_ct_cap) ret = cos_non_cc_cas((unsigned long *)&ctc->refcnt_flags, v, v + 1);
	else ret = cos_cas((unsigned long *)&ctc->refcnt_flags, v, v + 1);
	if (unlikely(ret != CAS_SUCCESS)) {
		/* undo before return */
		cos_throw(undo_ptc, -ECASFAIL);
	}
	
	compc = (struct cap_comp *)__cap_capactivate_pre(t, cap, capin, CAP_COMP, &ret);
	if (!compc) cos_throw(undo_ctc, ret);

	compc->entry_addr    = entry_addr;
	compc->info.pgtbl    = ptc->pgtbl;
	compc->info.captbl   = ctc->captbl;
	compc->info.comp_nfo = sa;
	compc->pgd           = ptc;
	compc->ct_top        = ctc;
	ltbl_get(lid, &compc->info.liveness);
	__cap_capactivate_post(&compc->h, CAP_COMP);

	return 0;

undo_ctc:
	if (pmem_ct_cap) cos_non_cc_faa((int *)&ctc->refcnt_flags, -1);
	else cos_faa((int *)&ctc->refcnt_flags, -1);
undo_ptc:
	if (pmem_pt_cap) cos_faa((int *)&ptc->refcnt_flags, -1);
	else cos_faa((int *)&ptc->refcnt_flags, -1);
	return ret;
}

static int comp_deactivate(struct cap_captbl *ct, capid_t capin, livenessid_t lid)
{ 
	int ret, pmem;
	struct cap_comp *compc;
	struct cap_pgtbl *pgd;
	struct cap_captbl *ct_top;

	compc = (struct cap_comp *)captbl_lkup(ct->captbl, capin);
	if (compc->h.type != CAP_COMP) return -EINVAL;

	ltbl_expire(&compc->info.liveness);
	pgd    = compc->pgd;
	ct_top = compc->ct_top;
	pmem = VA_IN_IVSHMEM_RANGE(pgd);
	if (pmem) assert(VA_IN_IVSHMEM_RANGE(ct_top));

	ret = cap_capdeactivate(ct, capin, CAP_COMP, lid); 
	if (ret) return ret;

	/* decrement the refcnt of the pgd, and top level of
	 * captbl. */
	if (pmem) {
		cos_non_cc_faa((int *)&pgd->refcnt_flags, -1);
		cos_non_cc_faa((int *)&ct_top->refcnt_flags, -1);
	} else {
		cos_faa((int *)&pgd->refcnt_flags, -1);
		cos_faa((int *)&ct_top->refcnt_flags, -1);
	}

	return 0;
}

static void comp_init(void)
{ assert(sizeof(struct cap_comp) <= __captbl_cap2bytes(CAP_COMP)); }

#endif /* COMPONENT_H */
