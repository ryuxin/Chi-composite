#include "rpc.h"
#include "test.h"

static struct msg_pool global_msg_pool;
static struct local_pos snt_pos[NUM_NODE], rcv_pos[NUM_NODE];
static struct shared_page ret_page[NUM_NODE];

void *
rpc_create(int node_mem, int size)
{
	int caller = node_mem & 0xFFFF, memid = node_mem >> 16, n;
	void *addr;
	volatile struct create_ret *ret = (struct create_ret *)ret_page[caller].addr;

	size  = round_up_to_page(size);
	n     = size/PAGE_SIZE;
	addr  = alloc_pages(n);
	memid = mem_create(addr, size);
	addr  = mem_retrieve(memid, caller);
	ret->addr = addr;
	ret->mem_id = memid;

	return ret_page[caller].dst;
}

int
rpc_connect(int node_mem, int recv_node, int size)
{
	int caller = node_mem & 0xFFFF, memid = node_mem >> 16;

	printc("rpc connect\n");
	return 0;
}

int
rpc_send(int snd_node, int rcv_node, int size, void *addr)
{
	int caller = snd_node, ret;
	struct msg_meta meta;

	clwb_range_opt(addr, addr+size);
	meta.addr = addr;
	meta.size = size;
	ret = msg_enqueue(&global_msg_pool.nodes[rcv_node].recv[caller], &meta);

	return ret;
}

void *
rpc_recv(int caller, int spin)
{
	int deq, i;
	struct msg_meta meta;
	void *addr;

	do {
		for(i=(caller+1)%NUM_NODE; i!=caller; i = (i+1)%NUM_NODE) {
			deq = msg_dequeue(&global_msg_pool.nodes[caller].recv[i], &meta);
			if (!deq) {
				addr = meta.addr;
				clflush_range(addr, addr+meta.size);
				return addr;
			}
		}
	} while(spin);
	return NULL;
}

int
rpc_free(int node_mem, int size)
{
	int caller = node_mem & 0xFFFF, memid = node_mem >> 16;

	printc("rpc free\n");
	return 0;
}

void
rpc_register(int node_mem)
{
	int caller = node_mem & 0xFFFF, memid = node_mem >> 16;
	void *addr, *dst;

	printc("rpc register caller %d\n", caller);

	addr = alloc_pages(1);
	dst  = alias_pages(caller, addr, 1);
	ret_page[caller].addr = addr;
	ret_page[caller].dst  = dst;
	((char *)addr)[4095] = '$';
}

void
rpc_init(int node_mem, vaddr_t untype, int size)
{
	int caller = node_mem & 0xFFFF, memid = node_mem >> 16;
	int i, j;
	vaddr_t vas = (vaddr_t)cos_get_heap_ptr()+PAGE_SIZE+COST_ARRAY_NUM_PAGE*PAGE_SIZE;
	vas = round_up_to_pgd_page(vas);

	printc("rpc init node %d addr %x size %x vas %x\n", caller, untype, size, vas);
	mem_mgr_init(untype, size, vas);
	memset((void *)&global_msg_pool, 0, sizeof(struct msg_pool));
#ifdef NO_HEAD
	memset((void *)&snt_pos, 0, sizeof(snt_pos));
	memset((void *)&rcv_pos, 0, sizeof(rcv_pos));
#endif
}

