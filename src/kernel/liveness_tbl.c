#include "include/liveness_tbl.h"

struct liveness_entry *__pmem_liveness_tbl;
void
ltbl_init(struct liveness_entry *l)
{
	int i;

	for (i = 0 ; i < LTBL_ENTS ; i++) {
		l[i].epoch = 0;
		l[i].deact_timestamp = 0;
	}
}
