/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "memcached.h"
#include "rpc.h"
#include "ps_global.h"
#include <bitmap.h>

#define TOT_MEM_SZ (1024*1024*320)
#define INIT_MEM_SZ (TOT_MEM_SZ/2)
#define ITEMS_PER_ALLOC 64
#define MC_SLAB_AOC_SZ (2*PAGE_SIZE)
#define BITMAP_SZ_PER_PAGE (PAGE_SIZE/sizeof(u32_t)*32*PAGE_SIZE)
#define BITMAP_SIZE ((TOT_MEM_SZ/BITMAP_SZ_PER_PAGE+1)*PAGE_SIZE)
#define WORDS_PER_PAGE (BITMAP_SIZE/sizeof(u32_t))
#define MAP_MAX WORDS_PER_PAGE

/* describes 2^(12+12+3 = 27) bytes */
struct spd_vas_occupied {
	u32_t pgd_occupied[WORDS_PER_PAGE];
};

struct vas_extent {
	void *base;
	struct spd_vas_occupied *map;
};

struct mc_status {
    int target, used, quiesce;  /* total, allocated and quiesce memory (#pages) */
    int set, get;      /* # of set and get requests */
    int alloc, free;   /* # of allocation and free  */
    int miss, init_done;
    int confict, evict;
    struct vas_extent trac;  /* track mc's own virtual address space */
    char pad[CACHE_LINE];
} __attribute__((aligned(CACHE_LINE), packed));

struct mc_mem_balance_info {
    int memid, sent;
    void *buf;
    int snt_num, rcv_num, pending_sent;
    char pad[CACHE_LINE];
} __attribute__((aligned(CACHE_LINE), packed));

struct mc_mem_msg {
	mc_message_t type;
    int num;
    void *pages[MC_MEM_BALANCE_MAX];
};

static struct mc_mem_balance_info balance[NUM_NODE/2];
static void *buf[NUM_NODE/2][NUM_NODE/2];
static int memid[NUM_NODE/2][NUM_NODE/2];
struct mc_status status[NUM_NODE/2];
static struct ps_qsc_list qsc_list[NUM_NODE/2];
#ifdef GET_RPC_TEST
PS_SLAB_CREATE_AFNS(item, MC_SLAB_OBJ_SZ, MC_SLAB_AOC_SZ, sizeof(struct ps_slab), mc_alloc_pages, mem_free_pages)
#else
PS_SLAB_CREATE_AFNS(item, MC_SLAB_OBJ_SZ, MC_SLAB_AOC_SZ, sizeof(struct ps_slab), mc_alloc_pages, mc_free_pages)
#endif
static struct shared_page key_page[NUM_NODE], data_page[NUM_NODE], ret_page[NUM_NODE];
static int mem_smr_reclaim(coreid_t curr, struct ps_qsc_list *ql);
static int mc_mem_evict(coreid_t curr, int npages);

static inline void
mc_status_init(struct mc_status *p, int target, int done)
{
    p->target = target;
    p->used = p->miss = 0;
    p->set = p->get = 0;
    p->alloc = p->free = p->quiesce = 0;
    p->confict = p->evict = 0;
    p->init_done = done;
}

struct ps_slab *
mc_alloc_pages(struct ps_mem *m, size_t sz, coreid_t coreid)
{
	struct ps_slab *s=NULL;
    int n, off;
	(void)m;

    assert(coreid < NUM_NODE/2);
    assert(sz == round_up_to_page(sz));

#ifndef GET_RPC_TEST
    if (status[coreid].used*100 > status[coreid].target*MC_MEM_EVICT_THOLD) {
        n = mem_smr_reclaim(coreid, &qsc_list[coreid]);
//        printc("node %d used %d qui %d tot %d smr slab %x ret %d\n", coreid, status[coreid].used, status[coreid].quiesce, status[coreid].target*MC_MEM_EVICT_THOLD/100, __ps_mem_item.percore[coreid].slab_info.el.list, n);
    }
    if (status[coreid].used*100 > status[coreid].target*MC_MEM_EVICT_THOLD) {
        n = status[coreid].used-status[coreid].target*(MC_MEM_EVICT_THOLD-2)/100;
        if (n > MC_MEM_EVICT_MAX) n = MC_MEM_EVICT_MAX;
        off = mc_mem_evict(coreid, n);
//        printc("node %d used %d qui %d tot %d evict %d slab %x ret %d\n", coreid, status[coreid].used, status[coreid].quiesce, status[coreid].target*MC_MEM_EVICT_THOLD/100, n, __ps_mem_item.percore[coreid].slab_info.el.list, off);
    }
#endif

    n = sz/PAGE_SIZE;
    off = bitmap_extent_find_set(&status[coreid].trac.map->pgd_occupied[0], 0, n, MAP_MAX);
    if (off < 0) {
        if (!status[coreid].init_done) s = (struct ps_slab *)alloc_pages(n);
        else {
            mc_print_status();
            assert(0);
        }
    } else {
        s = (struct ps_slab *)((char *)status[coreid].trac.base + off * PAGE_SIZE);
    }
	if (!s) return NULL;
	s->memory = s;
    status[coreid].alloc++;
    status[coreid].used += n;
	return s;
}

void
mc_free_pages(struct ps_mem *m, struct ps_slab *s, size_t sz, coreid_t coreid)
{
    int off, n;
    (void)m; 

#ifdef GET_RPC_TEST
    assert(0);
#endif

    assert(sz == round_up_to_page(sz));
    assert(s == (struct ps_slab *)round_up_to_page(s));
    assert(s->memsz == MC_SLAB_AOC_SZ);
    assert(coreid < NUM_NODE/2);
    assert(s->coreid == coreid);
    n = sz/PAGE_SIZE;
    rdtscll(s->tsc_free);
	__ps_qsc_enqueue(&qsc_list[coreid], s);
    status[coreid].free++;
    status[coreid].quiesce += n;
    return ;
}

void
mem_free_pages(struct ps_mem *m, struct ps_slab *s, size_t sz, coreid_t coreid)
{
    int off, n;
    (void)m; 

    assert(s == (struct ps_slab *)round_up_to_page(s));
    assert(coreid < NUM_NODE/2);

    n = sz/PAGE_SIZE;
    off = ((char *)s - (char *)status[coreid].trac.base) / PAGE_SIZE;
    assert(off + n < (int)(MAP_MAX * 32));
    bitmap_set_contig(&status[coreid].trac.map->pgd_occupied[0], off, n, 1);
    status[coreid].used -= n;
    return ;
}

static inline int
mem_quiesce(ps_tsc_t tsc, ps_tsc_t *qsc)
{
	*qsc = ps_tsc() - (ps_tsc_t)(MC_HASH_FLUSH_PEROID+MAX_MC_HASH_TIME);
	return 0;
}


static int
mem_smr_reclaim(coreid_t curr, struct ps_qsc_list *ql)
{
	struct ps_slab *a = __ps_qsc_peek(ql);
	int i=0;
	ps_tsc_t qsc, tsc;

#ifdef GET_RPC_TEST
    assert(0);
#endif

    if (!a) return i;
	tsc = a->tsc_free;
	if (mem_quiesce(tsc, &qsc)) return i;

	/* Remove a batch worth of items from the qlist */
	while (1) {
		a = __ps_qsc_peek(ql);
		if (!a || a->tsc_free > qsc) break;
        assert(a->coreid == curr);
		a = __ps_qsc_dequeue(ql);
        mem_free_pages(NULL, a, a->memsz, curr);
        status[curr].quiesce -= (a->memsz/PAGE_SIZE);
        i += (a->memsz/PAGE_SIZE);
	}

	return i;
}

static inline void
slab_evict(coreid_t curr, struct ps_slab *s)
{
	struct ps_mheader *alloc;
    item * it;
    int i, tot = ps_slab_nobjs_item(), obj_sz = __ps_slab_objmemsz(MC_SLAB_OBJ_SZ);

    assert(s->coreid == curr);
    assert(s->nfree == 0);
	alloc = (struct ps_mheader *)((char *)s->memory + sizeof(struct ps_slab));
    for(i=0; i<tot; i++, alloc = (struct ps_mheader *)((char *)alloc + obj_sz)) {
        if (alloc->type != SLAB_IN_USE) continue;
        it = (item *)__ps_mhead_mem(alloc);
        do_item_unlink(curr, it, it->hv);
        status[curr].evict++;
    }
}

static inline int
mc_mem_evict(coreid_t curr, int npages)
{
	struct ps_slab_info *si = &(__ps_mem_item.percore[curr].slab_info);
	struct ps_slab      *s  = si->el.list, *ms;
    int i=0;

#ifdef GET_RPC_TEST
    assert(0);
#endif

    while (i<npages && s) {
        i += (s->memsz/PAGE_SIZE);
        slab_evict(curr, s);
        s  = si->el.list;
    }
    return i;
}

static void
mem_slab_flush(struct ps_slab *s)
{
	struct ps_mheader *alloc;
    item * it;
    int i, tot = ps_slab_nobjs_item(), obj_sz = __ps_slab_objmemsz(MC_SLAB_OBJ_SZ);

	alloc = (struct ps_mheader *)((char *)s->memory + sizeof(struct ps_slab));
    for(i=0; i<tot; i++) {
        it = (item *)__ps_mhead_mem(alloc);
        assoc_flush(it->hv);
        alloc = (struct ps_mheader *)((char *)alloc + obj_sz);
    }
    clflush_range(s, (char *)s+s->memsz);
}

static void
mc_mem_sent(coreid_t cur, struct mc_mem_msg *msg)
{
    int n, i = 0;
    void *addr;
    struct mc_mem_msg *snt;

#ifdef MC_NO_MEM_BALANCE
    assert(0);
#endif

    if (status[cur].used*100 > status[cur].target*MC_MEM_BALANCE_THOLD) {
        balance[cur].pending_sent = msg->num;
        return ;
    }
    balance[cur].pending_sent = 0;
    balance[cur].snt_num++;
    n = status[cur].target*MC_MEM_BALANCE_THOLD/100 - status[cur].used;
    if (n > msg->num) n = msg->num;
    snt = (struct mc_mem_msg *)balance[cur].buf;
    snt->type = MC_MEM_REPLY;
    for(i=0; i<n; i++) {
        addr = (void *)mc_alloc_pages(NULL, PAGE_SIZE, cur);
        if (!addr) break;
        snt->pages[i] = addr;
        status[cur].alloc--;
        status[cur].used--;
        status[cur].target--;
    }
sent:
    snt->num = i;
	n = call_cap_mb(RPC_SEND, (balance[cur].memid << 16) | cur, !cur, sizeof(struct mc_msg));
	assert(!n);
}

static void
mc_mem_recv(coreid_t cur, struct mc_mem_msg *msg)
{
    int i, n = msg->num;

#ifdef MC_NO_MEM_BALANCE
    assert(0);
#endif

    assert(balance[cur].sent == 1);
    balance[cur].sent = 0;
    if (n) balance[cur].rcv_num++;
    for(i=0; i<n; i++) {
        mem_free_pages(NULL, (struct ps_slab *)(msg->pages[i]), PAGE_SIZE, cur);
        status[cur].used++;
        status[cur].target++;
    }
}

void *
parsec_mem_alloc(int node, int size)
{
    if (size != MC_SLAB_OBJ_SZ) {
        printc("mv ke %d v %d sz %d parsec %d\n", KEY_LENGTH, V_LENGTH, size, MC_SLAB_OBJ_SZ);
    }
    assert(size == MC_SLAB_OBJ_SZ);
    return ps_slab_alloc_item(node);
}

void
parsec_mem_free(int node, void *item)
{
    ps_slab_free_coreid_item(item, node);
}

/********************************* ITEM ACCESS *******************************/

/*
 * Allocates a new item.
 */
static inline item *
item_alloc(int node, char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes)
{
    item *it;
    /* do_item_alloc handles its own locks */
    it = do_item_alloc(node, key, nkey, flags, exptime, nbytes, 0);
    return it;
}

/*
 * Returns an item if it hasn't been marked as expired,
 * lazy-expiring as needed.
 */
item *
item_get(int node, const char *key, const size_t nkey)
{
    item *it;
    uint32_t hv;
    
    hv = hash(key, nkey);
    it = do_item_get(node, key, nkey, hv);
    
    return it;
}

item *
item_touch(int node, const char *key, size_t nkey, uint32_t exptime)
{
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    it = do_item_touch(node, key, nkey, exptime, hv);
    return it;
}

/*
 * Links an item into the LRU and hashtable.
 */
int
item_link(int node, item *item)
{
    int ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    ret = do_item_link(node, item, hv);
    return ret;
}

/*
 * Decrements the reference count on an item and adds it to the freelist if
 * needed.
 */
void
item_remove(int node, item *item)
{
    do_item_remove(node, item);
}

/*
 * Replaces one item with another in the hashtable.
 * Unprotected by a mutex lock since the core server does not require
 * it to be thread-safe.
 */
static inline int
item_replace(int node, item *old_it, item *new_it, const uint32_t hv)
{
    return do_item_replace(node, old_it, new_it, hv);
}

/*
 * Unlinks an item from the LRU and hashtable.
 */
void
item_unlink(int node, item *item)
{
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);
    do_item_unlink(node, item, hv);
}

/*
 * Moves an item to the back of the LRU queue.
 */
void
item_update(int node, item *item)
{
    do_item_update(node, item);
}

/* alloc + link / replace. Flattened from the state machine. */
int
mc_set_key_ext(int node, char *key, int nkey, char *data, int nbytes)
{
    item *old_it, *it;
    uint32_t hv;
    int retry = 1;

    status[node].set++;
start:
    /* alloc */
    it = item_alloc(node, key, nkey, 0, 0, nbytes+2);
    if (!it) {
        printc("ERROR: item_alloc failed once? \n");
        while(1) { ; }
        if (retry) {
            retry = 0;
             goto start;
        }

        return -1;
    }
    memcpy(ITEM_data(it), data, nbytes);

    /* link / replace next */
    hv = hash(ITEM_key(it), it->nkey);
    it->hv = hv;
    old_it = do_item_get(node, key, it->nkey, hv);

    if (old_it != NULL) {
        item_replace(node, old_it, it, hv);
        status[node].confict++;
    } else {
        do_item_link(node, it, hv);
    }

    return 0;
}

item *
mc_get_key_ext(int node, char *key, int nkey)
{
    item *it;
    status[node].get++;
    it = item_get(node, key, nkey);
    if (it) item_update(node, it);
    if (!it) {
        status[node].miss++;
        return NULL;
    }
    return it;
}

/********************************* MC INTERFACE *******************************/
void
mc_hashtbl_flush(int cur)
{
    int i;
	struct ps_slab *h, *s;
   
#ifdef GET_RPC_TEST
    return ;
#endif

    for(i=0; i<NUM_NODE/2; i++) {
        if (i != cur) cos_flush_cache(&qsc_list[i]);
        h = qsc_list[i].head;
        for(s=h; s; s = s->list.next) mem_slab_flush(s);
    }
}

void
mc_mem_balance(int cur)
{
    int n;
    struct mc_mem_msg *msg, tmsg;

#ifdef MC_NO_MEM_BALANCE
    return ;
#endif

    assert(cur < NUM_NODE/2);
    if (balance[cur].sent) return ;
    if (status[cur].used*100 <= status[cur].target*MC_MEM_BALANCE_THOLD) {
        if (balance[cur].pending_sent) {
            tmsg.num = balance[cur].pending_sent;
            mc_mem_sent(cur, &tmsg);
        }
        return ;
    }

    n = status[cur].used-status[cur].target*(MC_MEM_BALANCE_THOLD-1)/100;
    if (n > MC_MEM_BALANCE_MAX) n = MC_MEM_BALANCE_MAX;
    msg = (struct mc_mem_msg *)balance[cur].buf;
    msg->type = MC_MEM_REQ;
    msg->num  = n;
	n = call_cap_mb(RPC_SEND, (balance[cur].memid << 16) | cur, !cur, sizeof(struct mc_msg));
    balance[cur].sent = 1;
	assert(!n);
}

int
mc_set_key(int node, int nkey, int nbytes)
{
    int caller = node & 0xFFFF;
    char *key  = (char *)key_page[caller].addr;
    char *data = (char *)data_page[caller].addr;

    return mc_set_key_ext(node >> 16, key, nkey, data, nbytes);
}

void *
mc_get_key(int node, int nkey)
{
    item *it;
    int caller = node & 0xFFFF;
    char *key = (char *)key_page[caller].addr;

    node = node >> 16;
    it = mc_get_key_ext(node, key, nkey);
    if (!it) return NULL;
    memcpy(ret_page[caller].addr, ITEM_data(it), it->nbytes-2);

    return ret_page[caller].dst;
}

void
mc_print_status(void)
{
    int i, get;
    struct mc_status *p;

    for(i=0; i<NUM_NODE/2; i++) {
        p = &status[i];
        if (!p->get) get = 1;
        else get = p->get;
        printc("node %d target %d used %d quiesce %d alloc %d free %d balance snt %d rcv %d\n", 
                i, p->target, p->used, p->quiesce, p->alloc, p->free, balance[i].snt_num, balance[i].rcv_num);
        printc("set %d get %d miss %d prec(%%) %d confict %d evict %d\n",
                p->set, p->get, p->miss, p->miss*1000/get, p->confict, p->evict);
    }
}

void *
mc_register(int node, int type, int npage)
{
	int caller = node;
	void *addr, *dst;
    struct shared_page *page;

    switch (type) {
    case 1:
        page = &key_page[caller];
        break;
    case 2:
        page = &data_page[caller];
        break;
    case 3:
        page = &ret_page[caller];
        break;
    default:
        printc("mc register wrong type\n");
        return NULL;
    }
	addr = (void *)mc_alloc_pages(NULL, npage*PAGE_SIZE, 0);
	dst  = alias_pages(caller, addr, npage);
	page->addr = addr;
	page->dst  = dst;

    return dst;
}

void
mc_server_start(int cur)
{
	volatile struct recv_ret *rcv_ret;
	struct mc_msg *rcv, *ret;
    struct mc_mem_msg *msg;
    int r, sender, e = 1, set = 0, maxkf = 0;
	unsigned long long set_cost, send_cost, s1, s2, s3;
    unsigned long long prev0, prev1, curr;
    char *key, *data;
    item *it;

    cos_faa(&ivshmem_meta->boot_done, 1);
    printc("I am mc server node %d\n", cur);
	assert(cur < NUM_NODE/2);
    set_cost = send_cost = prev0 = prev1 = 0;
    kernel_flush();
    while (e) {
        rdtscll(curr);
        if (!prev0) prev0 = curr;
        if (curr - prev0 > MC_HASH_FLUSH_PEROID) {
            mc_hashtbl_flush(cur);
            r = kernel_flush();
			if (r > maxkf) maxkf = r;
            prev0 = curr;
        }
        if (!prev1) prev1 = curr;
        if (curr - prev1 > MC_MEM_BALANCE_PEROID) {
            mc_mem_balance(cur);
            prev1 = curr;
        }
   		rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur, 0, 0);
        if (!rcv_ret) continue;
		assert(rcv_ret);
        sender = rcv_ret->sender;
		rcv = (struct mc_msg *)(rcv_ret->addr);
		assert(rcv);
        key = (char *)rcv+sizeof(struct mc_msg);
        data = key+rcv->nkey;
        switch (rcv->type) {
        case MC_MSG_SET:
        {
            assert(sender >= NUM_NODE/2);
            rdtscll(s1);
            r = mc_set_key_ext(cur, key, rcv->nkey, data, rcv->nbytes);
            assert(!r);
            rdtscll(s2);
            ret = (struct mc_msg *)(buf[cur][sender-NUM_NODE/2]);
            ret->type = MC_MSG_SET_OK;
	    	r = call_cap_mb(RPC_SEND, (memid[cur][sender-NUM_NODE/2] << 16) | cur, sender, sizeof(struct mc_msg));
    		assert(!r);
            rdtscll(s3);
            set_cost += (s2-s1);
            send_cost += (s3-s2);
            set++;
            break;
        } 
        case MC_MSG_GET:
        {
#ifdef GET_RPC_TEST
            assert(sender >= NUM_NODE/2);
            it = mc_get_key_ext(cur, key, rcv->nkey);
            ret = (struct mc_msg *)(buf[cur][sender-NUM_NODE/2]);
            if (it) {
                ret->type = MC_MSG_GET_OK;
                ret->nbytes = it->nbytes-2;
                ret->data = (char *)&ret[1];
                memcpy(ret->data, ITEM_data(it), it->nbytes-2);
            } else {
                ret->type = MC_MSG_GET_FAIL;
                ret->nbytes = 0;
                ret->data = NULL;
            }
	    	r = call_cap_mb(RPC_SEND, (memid[cur][sender-NUM_NODE/2] << 16) | cur, sender, ret->nbytes+sizeof(struct mc_msg));
    		assert(!r);
#else
            assert(0);
#endif
            break;
        }
        case MC_MEM_REQ:
        {
            assert(sender == 1-cur);
            msg = (struct mc_mem_msg *)rcv;
            mc_mem_sent(cur, msg);
            break;
        }
        case MC_MEM_REPLY:
        {
            assert(sender == 1-cur);
            msg = (struct mc_mem_msg *)rcv;
            mc_mem_recv(cur, msg);
            break;
        }
        case MC_EXIT:
        {
            e = 0;
            break;
        }
        default:
        {
            break;
        }
        }
    }
    printc("MC server tracking set request %d avg %llu set %llu send %llu flush %d\n",
            set, (set_cost+send_cost)/set, set_cost/set, send_cost/set, maxkf);
    return ;
}

void
mc_init(int node, vaddr_t untype, int size)
{
    vaddr_t vas = (vaddr_t)cos_get_heap_ptr()+PAGE_SIZE+COST_ARRAY_NUM_PAGE*PAGE_SIZE;
    void *addr[NUM_NODE/2];
    int i, j, msg_sz = PAGE_SIZE;
   	struct create_ret *crt_ret;

    vas = round_up_to_pgd_page(vas)+PGD_SIZE;
    printc("mc init node %d addr %x size %dM vas %x\n", node, untype, size/(1024*1024), vas);
	mem_mgr_init(untype, size, vas);

	for(i=0; i<NUM_NODE/2; i++) memset(&qsc_list[i], 0, sizeof(struct ps_qsc_list));
    ps_slab_init_item();
    hash_init(JENKINS_HASH);
    assoc_init(0, 0);

    for(i=0; i<NUM_NODE/2; i++) {
	    call_cap_mb(RPC_REGISTER, i, 2, 3);
        for(j=0; j<NUM_NODE/2; j++) {
	        crt_ret = (struct create_ret *)call_cap_mb(RPC_CREATE, i, msg_sz, 3);
	        assert(crt_ret);
            buf[i][j]   = crt_ret->addr;
            memid[i][j] = crt_ret->mem_id;
            assert(buf[i][j]);
        }
    }

    memset(balance, 0, sizeof(balance));
	for(i=0; i<NUM_NODE/2; i++) {
        crt_ret = (struct create_ret *)call_cap_mb(RPC_CREATE, i, msg_sz, 3);
        assert(crt_ret);
        balance[i].buf   = crt_ret->addr;
        balance[i].memid = crt_ret->mem_id;
        assert(balance[i].buf);
    }

    for(i=0; i<NUM_NODE/2; i++) {
        mc_status_init(&status[i], INIT_MEM_SZ/PAGE_SIZE, 0);
        status[i].trac.base = (void *)vas;
        status[i].trac.map  = alloc_pages(BITMAP_SIZE/PAGE_SIZE);
        memset(status[i].trac.map, 0, BITMAP_SIZE);
        addr[i] = (void *)mc_alloc_pages(NULL, INIT_MEM_SZ, i);
    }

    for(i=0; i<NUM_NODE/2; i++) {
        mem_free_pages(NULL, (struct ps_slab *)addr[i], INIT_MEM_SZ, i);
        mc_status_init(&status[i], INIT_MEM_SZ/PAGE_SIZE, 1);
    }
    return ;
}
