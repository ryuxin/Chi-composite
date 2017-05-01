#ifndef RPC_H
#define RPC_H

#include "micro_booter.h"
#include "server_stub.h"
#include "mem_mgr.h"

#define MSG_NUM 8
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
	MC_GET_KEY      = 18,
	MC_SET_KEY      = 20,
	MC_INIT         = 22,
	MC_PRINT_STATUS = 24,
	MC_FLUSH        = 26,
	MC_DISCONNECT   = 28,
	MC_TEST         = 30,
	MC_PRELOAD      = 32,
	RPC_CAPTBL_FREE = round_up_to_pow2(MC_TEST, CAPMAX_ENTRY_SZ)
};

struct msg_meta {
	void *addr; 
	int size;   /* message size */
	int use;
} __attribute__((aligned(CACHE_LINE), packed));

struct msg_queue {
	int head;
	char pad[2*CACHELINE_SIZE-sizeof(int)];
	int tail;
	char _pad[2*CACHELINE_SIZE-sizeof(int)];
	struct msg_meta ring[MSG_NUM];
} __attribute__((aligned(2*CACHE_LINE), packed));

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
//int rpc_send(int node_mem, int recv_node, int size);
//void *rpc_recv(int node_mem, int spin);  /* return mem addr, mem_id, size and sender */ 
int rpc_free(int node_mem, int size);
void rpc_register(int node_mem);   /* set up shared page for return */
void rpc_init(int node_mem, vaddr_t untype, int size);

int rpc_send(int snd_node, int rcv_node, int size, void *addr);
void *rpc_recv(int caller, int spin);

DECLARE_INTERFACE(rpc_create)
DECLARE_INTERFACE(rpc_connect)
DECLARE_INTERFACE(rpc_send)
DECLARE_INTERFACE(rpc_recv)
DECLARE_INTERFACE(rpc_free)
DECLARE_INTERFACE(rpc_register)
DECLARE_INTERFACE(rpc_init)

/* single producer single consumer queue */
static inline int
msg_enqueue(struct msg_queue *q, struct msg_meta *entry)
{
	int consumer, producer, delta;

	consumer = cc_load_int(&q->head);	
	producer = q->tail;
	delta    = (producer + 1)%MSG_NUM;
	if (delta == consumer) {
		consumer = non_cc_load_int(&q->head);
		if (delta == consumer) return -1;
	}
	q->ring[producer] = *entry;
	clwb_range(&(q->ring[producer]), (char *)&(q->ring[producer]) + CACHE_LINE);
	non_cc_store_int(&q->tail, delta);
	return 0;
}

static inline int
msg_dequeue(struct msg_queue *q, struct msg_meta *entry)
{
	int consumer, producer;

	consumer = q->head;
	producer = cc_load_int(&q->tail);
	if (consumer == producer) {
		producer = non_cc_load_int(&q->tail);
		if (consumer == producer) return -1;
	}
	clflush_range(&q->ring[consumer], (char *)&q->ring[consumer] + CACHE_LINE);
	*entry = q->ring[consumer];
	non_cc_store_int(&q->head, (consumer+1)%MSG_NUM);
	return 0;
}

#endif /* RPC_H */
