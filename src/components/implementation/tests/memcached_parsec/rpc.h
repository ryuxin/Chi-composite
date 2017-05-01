#ifndef RPC_H
#define RPC_H

#include "micro_booter.h"
#include "server_stub.h"
#include "mem_mgr.h"

#define MSG_NUM 16
//#define NO_HEAD 1

enum rpc_captbl_layout {
	RPC_CREATE      = 2,
	RPC_CONNECT     = 4,
	RPC_SEND        = 6,
	RPC_RECV        = 8,
	RPC_FREE        = 10, 
	RPC_REGISTER    = 12,
	RPC_INIT        = 14,
	MC_REGISTER     = 16,
	MC_SET_KEY      = 18,
	MC_GET_KEY      = 20,
	MC_INIT         = 22,
	MC_PRINT_STATUS = 24,
	MC_FLUSH        = 26,
	MC_BALANCE      = 28,
	RPC_CAPTBL_FREE = round_up_to_pow2(MC_PRINT_STATUS, CAPMAX_ENTRY_SZ)
};

struct msg_meta {
	int mem_id; /* memory object id */
	int size;   /* message size */
	int use;
} __attribute__((aligned(CACHE_LINE), packed));

struct msg_queue {
#ifndef NO_HEAD
	int head;
	char pad[CACHELINE_SIZE-sizeof(int)];
	int tail;
	char _pad[CACHELINE_SIZE-sizeof(int)];
#endif
	struct msg_meta ring[MSG_NUM];
} __attribute__((aligned(CACHE_LINE), packed));

struct local_pos {
	int pos[NUM_NODE];
} __attribute__((aligned(CACHE_LINE), packed));

struct recv_queues {
	struct msg_queue recv[NUM_NODE];
} __attribute__((aligned(CACHE_LINE)));

struct msg_pool {
	struct recv_queues nodes[NUM_NODE];
};

struct create_ret {
	void *addr;
	int mem_id;
};

struct recv_ret {
	void *addr;
	int mem_id, size, sender;
};

struct shared_page {
	void *addr, *dst;
};

void *rpc_create(int node_mem, int size);   /* return mem address and mem_id*/
int rpc_connect(int node_mem, int recv_node, int size);
int rpc_send(int node_mem, int recv_node, int size);
void *rpc_recv(int node_mem, int spin);  /* return mem addr, mem_id, size and sender */ 
int rpc_free(int node_mem, int size);
void rpc_register(int node_mem);   /* set up shared page for return */
void rpc_init(int node_mem, vaddr_t untype, int size);

DECLARE_INTERFACE(rpc_create)
DECLARE_INTERFACE(rpc_connect)
DECLARE_INTERFACE(rpc_send)
DECLARE_INTERFACE(rpc_recv)
DECLARE_INTERFACE(rpc_free)
DECLARE_INTERFACE(rpc_register)
DECLARE_INTERFACE(rpc_init)

/* single producer single consumer queue */
#ifdef NO_HEAD
static inline int
msg_enqueue(struct msg_queue *q, int *pos, struct msg_meta *entry)
{
	struct msg_meta *m;
	int cur = *pos;

	m = &(q->ring[cur]);
	cos_flush_cache(m);
	if (m->use) return -1;
	m->mem_id = entry->mem_id;
	m->size   = entry->size;
	m->use    = 1;
	*pos      = (cur+1) & (MSG_NUM-1);
	cos_wb_cache(m);
	return 0;
}

static inline int
msg_dequeue(struct msg_queue *q, int *pos, struct msg_meta *entry)
{
	struct msg_meta *m;
	int cur = *pos;

	m = &(q->ring[cur]);
	cos_flush_cache(m);
	if (!m->use) return -1;
	entry->mem_id = m->mem_id;
	entry->size   = m->size;
	m->use        = 0;
	*pos          = (cur+1) & (MSG_NUM-1);
	cos_wb_cache(m);
	return 0;
}
#else
static inline int
msg_enqueue(struct msg_queue *q, struct msg_meta *entry)
{
	int consumer, producer, delta;

	consumer = non_cc_load_int(&q->head);
	producer = q->tail;
	delta = (producer + 1)%MSG_NUM;
	if (delta == consumer) return -1;
	q->ring[producer] = *entry;
	cos_wb_cache(&q->ring[producer]);
	non_cc_store_int(&q->tail, delta);
	return 0;
}

static inline int
msg_dequeue(struct msg_queue *q, struct msg_meta *entry)
{
	int consumer, producer;

	consumer = q->head;
	producer = non_cc_load_int(&q->tail);
	if (consumer == producer) return -1;
	cos_flush_cache(&q->ring[consumer]);
	*entry = q->ring[consumer];
	non_cc_store_int(&q->head, (consumer+1)%MSG_NUM);
	return 0;
}
#endif

#endif /* RPC_H */
