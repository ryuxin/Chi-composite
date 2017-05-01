/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "memcached.h"
#include "rpc.h"
#include "ps_global.h"
#include <bitmap.h>

#define TOT_MEM_SZ         (1024*1024*580)
#define INIT_MEM_SZ        (TOT_MEM_SZ/(NUM_NODE/2))
#define ITEMS_PER_ALLOC    64
#define MC_SLAB_AOC_SZ     (4*PAGE_SIZE)
#define BITMAP_SZ_PER_PAGE (PAGE_SIZE/sizeof(u32_t)*32*PAGE_SIZE)
#define BITMAP_SIZE        ((TOT_MEM_SZ/BITMAP_SZ_PER_PAGE+1)*PAGE_SIZE)
#define WORDS_PER_PAGE     (BITMAP_SIZE/sizeof(u32_t))
#define MAP_MAX            WORDS_PER_PAGE
#define SLAB_OBJ_TOT_SZ    ps_slab_objmem_item()
#define SLAB_FRAGMENTATION (MC_SLAB_AOC_SZ - ps_slab_nobjs_item()*SLAB_OBJ_TOT_SZ)

/* describes 2^(12+12+3 = 27) bytes */
struct spd_vas_occupied {
	u32_t pgd_occupied[WORDS_PER_PAGE];
};

struct vas_extent {
	void *base;
	struct spd_vas_occupied *map;
};

struct mc_status {
    unsigned long long target, used, quiesce;  /* total, allocated and quiesce memory (#bytes) */
    int get, set;      /* # of set and get requests */
    int alloc, free;   /* # of allocation and free  */
    int confict, evict;
    struct vas_extent trac;  /* track mc's own virtual address space */
    char pad[CACHE_LINE];
} __attribute__((aligned(CACHE_LINE), packed));

struct mc_mem_balance_info {
    int sent;
    void *buf;
    int snt_num, rcv_num, pending_sent;
    char pad[CACHE_LINE];
} __attribute__((aligned(CACHE_LINE), packed));

struct mc_mem_msg {
	mc_message_t type;
    int id, num;
    void *pages[MC_MEM_BALANCE_MAX];
};

#define QUIS_RING 1
#ifdef QUIS_RING
#define QUIS_NUM 11264
#define QUIS_PAGE_NUM (QUIS_NUM*sizeof(struct ps_mheader *)/PAGE_SIZE)
struct ps_qsc_ring {
    int head, tail;
    struct ps_mheader **ring;
};

static struct ps_qsc_ring qsc_list[NUM_NODE/2];
static int mem_smr_reclaim(coreid_t curr, struct ps_qsc_ring *ql);
#else
static struct ps_qsc_list qsc_list[NUM_NODE/2];
static int mem_smr_reclaim(coreid_t curr, struct ps_qsc_list *ql);
#endif

int loading;
static struct mc_mem_balance_info balance[NUM_NODE/2];
static void *msg_buf[NUM_NODE][NUM_NODE];
struct mc_status status[NUM_NODE/2];
PS_SLAB_CREATE_AFNS(item, MC_SLAB_OBJ_SZ, MC_SLAB_AOC_SZ, sizeof(struct ps_slab), mc_alloc_pages, mem_free_pages)
static struct shared_page key_page[NUM_NODE], data_page[NUM_NODE], ret_page[NUM_NODE];
static int mc_mem_evict(coreid_t curr, int npages);

#define LATENCY_TRACKING 1 
#ifdef LATENCY_TRACKING
struct Latency {
    unsigned long long n, c;
}__attribute__((aligned(2*CACHE_LINE), packed));
struct Latency records[NUM_NODE];
#endif

static inline void
mc_status_init(struct mc_status *p, unsigned long long target)
{
    p->target = target;
    p->set = p->get = p->used = 0;
    p->alloc = p->free = p->quiesce = 0;
    p->confict = p->evict = 0;
}

struct ps_slab *
mc_alloc_pages(struct ps_mem *m, size_t sz, coreid_t coreid)
{
	struct ps_slab *s=NULL;
    int n, off, smr=0, evt=0;
	(void)m;

    assert(sz == round_up_to_page(sz));

    if (status[coreid].used*100 > status[coreid].target*MC_MEM_EVICT_THOLD) {
        smr = mem_smr_reclaim(coreid, &qsc_list[coreid]);
    }
    if (status[coreid].used*100 > status[coreid].target*MC_MEM_EVICT_THOLD) {
#ifdef MC_NO_MEM_BALANCE
        if (loading) {
            mc_print_status();
            return NULL;
        }
#endif
        evt = mc_mem_evict(coreid, 1);
    }

    n = sz/PAGE_SIZE;
    off = bitmap_extent_find_set(&status[coreid].trac.map->pgd_occupied[0], 0, n, MAP_MAX);
    if (off >= 0) s = (struct ps_slab *)((char *)status[coreid].trac.base + off * PAGE_SIZE);
    else if (!smr) mc_print_status();
	if (!s) return NULL;
	s->memory = s;
    status[coreid].alloc++;
    status[coreid].used += SLAB_FRAGMENTATION;
	return s;
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
    status[coreid].free++;
    status[coreid].used -= SLAB_FRAGMENTATION;
    return ;
}

static inline int
mem_quiesce(ps_tsc_t tsc, ps_tsc_t *qsc)
{
	*qsc = ps_tsc() - (ps_tsc_t)(MC_HASH_FLUSH_PEROID+MAX_MC_HASH_TIME);
	return 0;
}

#ifdef QUIS_RING
static inline struct ps_mheader *
qsc_ring_peek(struct ps_qsc_ring *ql)
{
    return ql->ring[ql->head];
}

static inline struct ps_mheader *
qsc_ring_dequeue(struct ps_qsc_ring *ql)
{
    struct ps_mheader *ret = ql->ring[ql->head];

    if (ret) {
        ql->ring[ql->head] = NULL;
        ql->head = (ql->head+1) % QUIS_NUM;
    }
    return ret;
}

static inline void
qsc_ring_enqueue(struct ps_qsc_ring *ql, struct ps_mheader *m)
{
    if (ql->head == (ql->tail + 1) % QUIS_NUM) {
        int r=mem_smr_reclaim(ql - qsc_list, ql);
        if (ql->head == (ql->tail + 1) % QUIS_NUM) {
            printc("quisence ring full %d reclaim %d node %d\n", QUIS_NUM, r, (int)(ql - qsc_list));
            assert(0);
        }
    }
    ql->ring[ql->tail] = m;
    ql->tail = (ql->tail+1) % QUIS_NUM;
}

static inline void
qsc_ring_flush(struct ps_qsc_ring *ql)
{
#define FETCH_LEN 32
    int i, j;
    struct ps_mheader *s;
    item * it;
    uint32_t hv;

    for(i=ql->head; i != ql->tail; i = (i+1)%QUIS_NUM) {
        s = ql->ring[i];
        if (i % FETCH_LEN == 0) {
            j = (i + FETCH_LEN) % QUIS_NUM;
            __builtin_prefetch(&(ql->ring[j]), 0, 0);
        }
        if (s == NULL) continue;
        it = (item *)__ps_mhead_mem(s);
        hv = it->hv;
        clflush_range_opt(s, (char *)s + SLAB_OBJ_TOT_SZ);
        assoc_flush_opt(hv, it);
        /* assoc_flush(hv); */
    }
}
#endif

#ifdef QUIS_RING
static int
mem_smr_reclaim(coreid_t curr, struct ps_qsc_ring *ql)
{
	struct ps_mheader *a = qsc_ring_peek(ql);
	int i=0;
	ps_tsc_t qsc, tsc;

    if (!a) return i;
	tsc = a->tsc_free;
	if (mem_quiesce(tsc, &qsc)) return i;

	while (1) {
		a = qsc_ring_peek(ql);
		if (!a || a->tsc_free > qsc) break;

        assert(a->slab->coreid == curr);
		a = qsc_ring_dequeue(ql);
#else
static int
mem_smr_reclaim(coreid_t curr, struct ps_qsc_list *ql)
{
	struct ps_mheader *a = __ps_qsc_peek(ql);
	int i=0;
	ps_tsc_t qsc, tsc;

    if (!a) return i;
	tsc = a->tsc_free;
	if (mem_quiesce(tsc, &qsc)) return i;

	while (1) {
		a = __ps_qsc_peek(ql);
		if (!a || a->tsc_free > qsc) break;

        assert(a->slab->coreid == curr);
		a = __ps_qsc_dequeue(ql);
#endif
        ps_slab_free_coreid_item(__ps_mhead_mem(a), curr);
        i += SLAB_OBJ_TOT_SZ;
	}
    status[curr].used    -= i;
    status[curr].quiesce -= i;

	return i;
}

static inline int
slab_evict(coreid_t curr, struct ps_slab *s)
{
	struct ps_mheader *alloc;
    item * it;
    int i, tot = ps_slab_nobjs_item(), e = 0;

    assert(s->coreid == curr);
    assert(s->nfree == 0);
	alloc = (struct ps_mheader *)((char *)s->memory + sizeof(struct ps_slab));
    for(i=0; i<tot; i++, alloc = (struct ps_mheader *)((char *)alloc + SLAB_OBJ_TOT_SZ)) {
        if (alloc->tsc_free != SLAB_IN_USE) continue;
        it = (item *)__ps_mhead_mem(alloc);
        assert(__mc_hv2node(it->hv) == curr);
        do_item_unlink(curr, it, it->hv);
        e++;
    }
    status[curr].evict += e;
    return e;
}

static inline int
mc_mem_evict(coreid_t curr, int npages)
{
	struct ps_slab_info *si = &(__ps_mem_item.percore[curr].slab_info);
	struct ps_slab      *s  = si->el.list;
    int i=0, r;

    while (i<npages && s) {
        i += (s->memsz/PAGE_SIZE);
        r = slab_evict(curr, s);
        s  = ps_list_next(s, list);
    }
    s  = si->fl.list;
    while (i<npages && s) {
        i += (s->memsz/PAGE_SIZE);
        slab_evict(curr, s);
        s  = ps_list_next(s, list);
    }
    return i;
}

static inline void
mem_slab_flush(struct ps_mheader *s)
{
    item * it;

    it = (item *)__ps_mhead_mem(s);
    assoc_flush_opt(it->hv, it);
    clflush_range(s, (char *)s + SLAB_OBJ_TOT_SZ);
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
    n = (int)(status[cur].target*MC_MEM_BALANCE_THOLD/100 - status[cur].used);
    n /= PAGE_SIZE;
    if (n > msg->num) n = msg->num;
    snt       = (struct mc_mem_msg *)balance[cur].buf;
    snt->type = MC_MEM_REPLY;
    snt->id   = cur;
    for(i=0; i<n; i++) {
        addr = (void *)mc_alloc_pages(NULL, PAGE_SIZE, cur);
        if (!addr) break;
        snt->pages[i] = addr;
        status[cur].alloc--;
        status[cur].used -= SLAB_FRAGMENTATION;
        status[cur].target -= PAGE_SIZE;
    }
    snt->num = i;
    n = rpc_send(cur, !cur, sizeof(struct mc_mem_msg), snt);
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
        status[cur].free++;
        status[cur].used += SLAB_FRAGMENTATION;
        status[cur].target += PAGE_SIZE;
    }
}

void *
parsec_mem_alloc(int node, int size)
{
    if (size != MC_SLAB_OBJ_SZ) {
        printc("mv ke %d v %d sz %d parsec %d\n", KEY_LENGTH, V_LENGTH, size, MC_SLAB_OBJ_SZ);
    }
    assert(size == MC_SLAB_OBJ_SZ);
    status[node].used += SLAB_OBJ_TOT_SZ;
    return ps_slab_alloc_item(node);
}

void
parsec_mem_free(int node, void *item)
{
	struct ps_mheader  *m  = __ps_mhead_get(item);
    unsigned long long tsc;

    rdtscll(tsc);
	__ps_mhead_setfree(m, tsc);
#ifdef QUIS_RING
    qsc_ring_enqueue(&qsc_list[node], m);
#else
    __ps_qsc_enqueue(&qsc_list[node], m);
#endif
    status[node].quiesce += SLAB_OBJ_TOT_SZ;
}

int
mc_set_key_rcv(int node, char *key, int nkey, char *data, int nbytes, uint32_t hv)
{
    item *old_it = NULL, *it = NULL;

    status[node].set++;

    it = do_item_alloc(node, key, nkey, 0, 0, nbytes+2, 0);
    if (!it) {
        printc("ERROR: item_alloc failed once? loading %d\n", loading);
        return -1;
    }
    memcpy(ITEM_data(it), data, nbytes);
    it->hv = hv;

    old_it = do_item_get(node, key, nkey, hv);
    if (old_it != NULL) {
        do_item_replace(node, old_it, it, hv);
        status[node].confict++;
    } else {
        do_item_link(node, it, hv);
    }
    return 0;
}

int
mc_set_key_ext(int caller, int node, char *key, int nkey, char *data, int nbytes, uint32_t hv)
{
    int r = 0;

#ifdef SET_RPC_TEST
    struct mc_msg *msg, *ret;
    msg         = (struct mc_msg *)(msg_buf[caller][node]);
    msg->type   = MC_MSG_SET;
    msg->nkey   = nkey;
    msg->nbytes = nbytes;
    msg->id     = caller;
    msg->hv     = hv;
    msg->key    = key;
    msg->data   = data;
    clwb_range_opt(msg->key, msg->key + nkey);
    clwb_range_opt(msg->data, msg->data + nbytes);

    r = rpc_send(caller, node, sizeof(struct mc_msg), msg);
    assert(!r);
    ret = (struct mc_msg *)rpc_recv(caller, 1);
    assert(ret);
    assert(ret->id == node);
#ifdef KEY_SKEW
    if (ret->type == MC_MSG_SET_FAIL) r = -1;
    else if (ret->type != MC_MSG_SET_OK) assert(0);
#else
    assert(ret->type == MC_MSG_SET_OK);
#endif
#else
    r = mc_set_key_rcv(node, key, nkey, data, nbytes, hv);
#endif
    return r;
}

char *
mc_get_key_ext(char *key, int nkey, int *nbyte)
{
    item *it;
    int node;
    uint32_t hv;

#ifdef LATENCY_TRACKING
    unsigned long long lts, lte;
    rdtscll(lts);
#endif

    hv   = hash(key, nkey);
    node = __mc_hv2node(hv);
    status[node].get++;
    it = do_item_get(node, key, nkey, hv);
    if (it) do_item_update(node, it);
    else {
        assoc_flush(hv);
        it = do_item_get(node, key, nkey, hv);
        if (it) do_item_update(node, it);
    }
/* #define UPDATE_RA (50) */
/* #define MODULE (100 - UPDATE_RA) */
/* if ((status[node].get % MODULE) < UPDATE_RA * (NUM_NODE/2)) { */
/*     assoc_flush(hv); */
/*     it = do_item_get(node, key, nkey, hv); */
/*     assert(it); */
/* } */
#ifdef LATENCY_TRACKING
    rdtscll(lte);
    assert(lte > lts);
    if (!loading) {
        records[node].n++;
        records[node].c += (lte - lts);
    }
#endif
    if (it) {
        *nbyte = it->nbytes-2;
        return ITEM_data(it);
    } else {
        *nbyte = 0;
        return NULL;
    }
}

/********************************* MC INTERFACE *******************************/
int
mc_hashtbl_flush(int cur)
{
    int i, r = 0;

#ifndef SET_RPC_TEST
    mem_smr_reclaim(0, &qsc_list[0]);
#endif
    /* assoc_flush_tbl(); */
    /* for(i=0; i<NUM_NODE/2; i++) { */
    /* 	struct ps_slab_info *si = &(__ps_mem_item.percore[i].slab_info); */
    /* 	struct ps_slab      *s, *n, *h; */
    /*     h = s = si->el.list; */
    /*     if (s) { */
    /*         do { */
    /*             r += s->memsz; */
    /*             n  = ps_list_next(s, list); */
    /*             clflush_range_opt(s, (char *)s + s->memsz); */
    /*             s = n; */
    /*         } while(s != h); */
    /*     } */
    /*     h = s = si->fl.list; */
    /*     if (s) { */
    /*         do { */
    /*             r += s->memsz; */
    /*             n  = ps_list_next(s, list); */
    /*             clflush_range_opt(s, (char *)s + s->memsz); */
    /*             s = n; */
    /*         } while(s != h); */
    /*     } */
    /* } */
	/* asm volatile ("sfence"); */
    /* return r/PAGE_SIZE; */

	struct ps_mheader *h, *s;
#ifdef QUIS_RING
    for(i=0; i<NUM_NODE/2; i++) {
        if (i != cur) clflush_range_opt(qsc_list[i].ring, (char *)(qsc_list[i].ring) + QUIS_PAGE_NUM*PAGE_SIZE);
    }
    for(i=0; i<NUM_NODE/2; i++) {
        r += (QUIS_NUM + qsc_list[i].tail - qsc_list[i].head) % QUIS_NUM;
    }
	asm volatile ("sfence");
    for(i=0; i<NUM_NODE/2; i++) {
        qsc_ring_flush(&qsc_list[i]);
    }
	asm volatile ("sfence");
    return r;
#else
    for(i=0; i<NUM_NODE/2; i++) {
        if (i != cur) clflush_range(&qsc_list[i], (char *)&qsc_list[i] + CACHE_LINE);
        h = qsc_list[i].head;
        for(s=h; s; s = s->next) {
            mem_slab_flush(s);
        }
    }
#endif
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

    n = (int)(status[cur].used-status[cur].target*(MC_MEM_BALANCE_THOLD-1)/100);
    n /= PAGE_SIZE;
    if (n > MC_MEM_BALANCE_MAX) n = MC_MEM_BALANCE_MAX;
    msg = (struct mc_mem_msg *)balance[cur].buf;
    msg->type = MC_MEM_REQ;
    msg->num  = n;
    msg->id   = cur;
    n = rpc_send(cur, !cur, sizeof(struct mc_mem_msg), msg);
    balance[cur].sent = 1;
	assert(!n);
}

int
mc_set_key(int caller, int nkey, int nbytes)
{
    int node, r;
    uint32_t hv;
    char *key  = (char *)key_page[caller].addr;
    char *data = (char *)key_page[caller].addr + nkey;

/* #ifdef LATENCY_TRACKING */
/*     unsigned long long lts, lte; */
/*     rdtscll(lts); */
/* #endif */

    hv   = hash(key, nkey);
    node = __mc_hv2node(hv);
    r    = mc_set_key_ext(caller, node, key, nkey, data, nbytes, hv);

/* #ifdef LATENCY_TRACKING */
/*     rdtscll(lte); */
/*     assert(lte > lts); */
/*     if (!loading) { */
/*         records[caller].n++; */
/*         records[caller].c += (lte - lts); */
/*     } */
/* #endif */

    return r;
}

void *
mc_get_key(int caller, int nkey)
{
    int nbyte;
    char *value;
    char *key = (char *)key_page[caller].addr;

/*#ifdef LATENCY_TRACKING*/
/*    unsigned long long lts, lte;*/
/*    rdtscll(lts);*/
/*#endif*/

    value = mc_get_key_ext(key, nkey, &nbyte);
    if (!value) return NULL;
    memcpy(ret_page[caller].addr, value, nbyte);

/*#ifdef LATENCY_TRACKING*/
/*    rdtscll(lte);*/
/*    assert(lte > lts);*/
/*    if (!loading) {*/
/*        records[caller].n++;*/
/*        records[caller].c += (lte - lts);*/
/*    }*/
/*#endif*/

    return ret_page[caller].dst;
}

void
mc_print_status(void)
{
    int i;
    struct mc_status *p;

    for(i=0; i<NUM_NODE/2; i++) {
        p = &status[i];
        printc("node %d target %llu used %lluM quiesce %llu alloc %d free %d balance snt %d rcv %d\n", 
               i, p->target/PAGE_SIZE, p->used/1024/1024, p->quiesce/PAGE_SIZE, p->alloc, p->free, balance[i].snt_num, balance[i].rcv_num);
        printc("set %d get %d confict %d evict %d\n", p->set, p->get, p->confict, p->evict);
    }
#ifdef LATENCY_TRACKING
    for(i=0; i<NUM_NODE; i++) {
        unsigned long long c;
        if (records[i].n) c = records[i].c / records[i].n;
        else c = 0;
        printc("node %d track num %llu cost %llu\n", i, records[i].n, c);
    }
#endif
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
	addr = (void *)alloc_pages(npage);
	dst  = alias_pages(caller, addr, npage);
	page->addr = addr;
	page->dst  = dst;

    return dst;
}

void
mc_preload_key(int cur)
{
	client_load(cur);
}

void
mc_test(int cur)
{
	client_bench(cur);
}

void
mc_disconnect(int caller, int server)
{
    if (server == MC_LOADING) {
        loading = 0;
        return ;
    }
	struct mc_msg *msg = (struct mc_msg *)(msg_buf[caller][server]);

	msg->type = MC_EXIT;
    msg->id   = caller;
    rpc_send(caller, server, sizeof(struct mc_msg), msg);
}

void
mc_server_start(int cur)
{
	struct mc_msg *rcv, *ret;
    struct mc_mem_msg *msg;
    int r, sender, e = 1, no = 0;
    unsigned long long prev0, prev1, curr;
    char *key, *data;
    item *it;

#ifdef LATENCY_TRACKING
    unsigned long long db, ds, de, dc;
    db = dc = 0;
#endif
    prev0 = prev1 = 0;
    cos_faa(&ivshmem_meta->boot_done, 1);
    printc("I am mc server node %d\n", cur);
	assert(cur < NUM_NODE/2);
    kernel_flush();
#ifndef SET_RPC_TEST
    return 0;
#endif
    while (e) {
        rdtscll(curr);
        if (!prev0) prev0 = curr;
        if (curr - prev0 > MC_HASH_FLUSH_PEROID) {
            mem_smr_reclaim(cur, &qsc_list[cur]);
            no += mc_hashtbl_flush(cur);
            r = kernel_flush();
            prev0 = curr;
#ifdef LATENCY_TRACKING
            rdtscll(de);
            db++;
            dc += (de - curr);
#endif
        }
        if (!prev1) prev1 = curr;
        if (curr - prev1 > MC_MEM_BALANCE_PEROID) {
            mem_smr_reclaim(cur, &qsc_list[cur]);
            mc_mem_balance(cur);
            prev1 = curr;
        }

        rcv = (struct mc_msg *)rpc_recv(cur, 0);
        if (!rcv) continue;
        sender = rcv->id;
        key    = rcv->key;
        data   = rcv->data;
        switch (rcv->type) {
        case MC_MSG_SET:
        {
            assert(sender >= NUM_NODE/2);
/* #ifdef LATENCY_TRACKING */
/*             unsigned long long lts, lte; */
/*             rdtscll(lts); */
/* #endif */
            clflush_range_opt(key, key + rcv->nkey);
            clflush_range(data, data + rcv->nbytes);
            r = mc_set_key_rcv( cur, key, rcv->nkey, data, rcv->nbytes, rcv->hv);
#ifndef KEY_SKEW
            assert(!r);
#endif
/* #ifdef LATENCY_TRACKING */
/*             rdtscll(lte); */
/*             assert(lte > lts); */
/*             if (!loading) { */
/*                 records[cur].n++; */
/*                 records[cur].c += (lte - lts); */
/*             } */
/* #endif */
            ret       = (struct mc_msg *)(msg_buf[cur][sender]);
            ret->id   = cur;
            if (!r) ret->type = MC_MSG_SET_OK;
            else    ret->type = MC_MSG_SET_FAIL;
            r         = rpc_send(cur, sender, sizeof(struct mc_msg), ret);
    		assert(!r);
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
            assert(sender >= NUM_NODE/2);
            e = 0;
            break;
        }
        case MC_MSG_GET:
        {
            assert(0);
            break;
        }
        default:
        {
            break;
        }
        }
    }
#ifdef LATENCY_TRACKING
    printc("server flush %llu cost %llu amtor %llu object %d per %llu\n", db, dc/db, dc/(unsigned long long )5000000, no, dc/(unsigned long long )no);
#endif
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

#ifdef QUIS_RING
	for(i=0; i<NUM_NODE/2; i++) {
        memset(&qsc_list[i], 0, sizeof(struct ps_qsc_ring));
        qsc_list[i].ring = (struct ps_mheader **)alloc_pages(QUIS_PAGE_NUM);
        memset(qsc_list[i].ring, 0, QUIS_PAGE_NUM*PAGE_SIZE);
    }
#else
	for(i=0; i<NUM_NODE/2; i++) memset(&qsc_list[i], 0, sizeof(struct ps_qsc_list));
#endif

#ifdef LATENCY_TRACKING
    memset(records, 0, sizeof(records));
#endif

    ps_slab_init_item();
    hash_init(JENKINS_HASH);
    assoc_init(0, 0);

    for(i=0; i<NUM_NODE; i++) {
        for(j=0; j<NUM_NODE; j++) {
            msg_buf[i][j] = alloc_pages(1);
        }
    }

    memset(status, 0, sizeof(status));
    memset(balance, 0, sizeof(balance));
	for(i=0; i<NUM_NODE/2; i++) {
        balance[i].buf = alloc_pages(1);
    }

    for(i=0; i<NUM_NODE/2; i++) {
        status[i].trac.map  = alloc_pages(BITMAP_SIZE/PAGE_SIZE);
        memset(status[i].trac.map, 0, BITMAP_SIZE);
    }
    for(i=0; i<NUM_NODE/2; i++) {
        addr[i] = (void *)alloc_pages(INIT_MEM_SZ/PAGE_SIZE);
        status[i].trac.base = addr[0];
    }

    for(i=0; i<NUM_NODE/2; i++) {
        mem_free_pages(NULL, (struct ps_slab *)addr[i], INIT_MEM_SZ, i);
        mc_status_init(&status[i], INIT_MEM_SZ);
    }

    loading = 1;
    return ;
}
