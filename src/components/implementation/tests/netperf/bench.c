#include "bench.h"
#include "rpc.h"

#define PACKET_SIZE (8192)
#define BUFFER_SIZE (2+MSG_NUM)
#define TOT_SIZE (1<<30)
#define TOT_RR_REQ (1000000)
enum {
	RPC_STREAM,
	RPC_RR,
	RPC_END
};
struct mem_obj {
	void *buf;
	int memid;
};

struct mem_obj buffer[BUFFER_SIZE];
int test_type = RPC_RR, snt_id, rcv_id;
void *snt_buf, *rcv_buf;
char rr_buf[PACKET_SIZE];

static void
rpc_stream_sent(void)
{
	struct create_ret *crt_ret;
	struct recv_ret *rcv_ret;
	int i, msg_sz, n, memid, *rcv, r;
	unsigned long long start, end, time;

	msg_sz = round_up_to_page(PACKET_SIZE);
	for(i=0; i<BUFFER_SIZE; i++) {
		crt_ret = (struct create_ret *)call_cap_mb(RPC_CREATE, cur_node, msg_sz, 0);
		buffer[i].buf   = crt_ret->addr;
		buffer[i].memid = crt_ret->mem_id;
		assert(buffer[i].buf);
		memset(buffer[i].buf, '$', PACKET_SIZE);
		((char *)buffer[i].buf)[PACKET_SIZE-1] = '\0';
	}

	n = TOT_SIZE/PACKET_SIZE;
	rdtscll(start);
	for(i=0; i<n; i++) {
		memid = buffer[i%BUFFER_SIZE].memid;
		do {
			r = call_cap_mb(RPC_SEND, (memid << 16) | cur_node, 1-cur_node, PACKET_SIZE);
		} while(r);
		assert(!r);
	}
	rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1-cur_node, 0);
	rcv = (int *)(rcv_ret->addr);
	assert(rcv[0] == RPC_END);
	rdtscll(end);

	time = (end - start)/(unsigned long long)CPU_FREQ;
	printc("n %d tot size %dbytes tiem %llums thput %llu Mbits/s\n", n, TOT_SIZE, time, (unsigned long long)TOT_SIZE /1000*8 / time);
}

static void
rpc_stream_recv(void)
{
	int i, n;
	struct recv_ret *rcv_ret;
	void *rcv;

	n = TOT_SIZE/PACKET_SIZE;
	for(i=0; i<n; i++) {
		rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1, 0);
		assert(rcv_ret);
		assert(rcv_ret->sender == 1-cur_node);
		assert(rcv_ret->size == PACKET_SIZE);
		assert(rcv_ret->addr);
//		memcpy(rr_buf, rcv_ret->addr, PACKET_SIZE);
	}
	((int *)snt_buf)[0] = RPC_END;
	call_cap_mb(RPC_SEND, (snt_id << 16) | cur_node, 1-cur_node, sizeof(int));
}

static void
rpc_rr_sent()
{
	struct create_ret *crt_ret;
	struct recv_ret *rcv_ret;
	int i, msg_sz, memid, r;
	unsigned long long start, end, time;

	msg_sz = round_up_to_page(PACKET_SIZE);
	crt_ret = (struct create_ret *)call_cap_mb(RPC_CREATE, cur_node, msg_sz, 0);
	rcv_buf = crt_ret->addr;
	memid   = crt_ret->mem_id;
	assert(rcv_buf);
	memset(rcv_buf, '$', PACKET_SIZE);
	((char *)rcv_buf)[PACKET_SIZE-1] = '\0';

	rdtscll(start);
	for(i=0; i<TOT_RR_REQ; i++) {
		//rcv_ret = (struct recv_ret *)call_cap_mb(RPC_CALL, (memid << 16) | cur_node, 1-cur_node, PACKET_SIZE);
		r = call_cap_mb(RPC_SEND, (memid << 16) | cur_node, 1-cur_node, PACKET_SIZE);
		assert(!r);
		rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1, 0);
		assert(rcv_ret);
		assert(rcv_ret->sender == 1-cur_node);
		assert(rcv_ret->size == PACKET_SIZE);
		assert(rcv_ret->addr);
	}
	rdtscll(end);

	time = (end - start)/(unsigned long long)CPU_FREQ;
	printc("tot trac %d tiem %llums thput %llu trac/s\n", TOT_RR_REQ, time, (unsigned long long)TOT_RR_REQ * 1000 / time);
}

static void
rpc_rr_recv()
{
	int i, r;
	struct recv_ret *rcv_ret;

	for(i=0; i<TOT_RR_REQ; i++) {
		//r = call_cap_mb(RPC_WAIT, (snt_id << 16) | cur_node, PACKET_SIZE, 0);
		rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1, 0);
		assert(rcv_ret);
		assert(rcv_ret->sender == 1-cur_node);
		assert(rcv_ret->size == PACKET_SIZE);
		assert(rcv_ret->addr);
		r = call_cap_mb(RPC_SEND, (snt_id << 16) | cur_node, 1-cur_node, PACKET_SIZE);
		assert(!r);
	}
}

void
client_start(int cur)
{
	struct recv_ret *rcv_ret;
	int *rcv;

	cur_node = cur;
	printc("I am client heap %p cur_node %d\n", cos_get_heap_ptr(), cur_node);
	printc("meta %p magic %s done %d\n", ivshmem_meta, ivshmem_meta->magic, ivshmem_meta->boot_done);
	call_cap_mb(RPC_REGISTER, cur_node, 0, 0);

	rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1-cur_node, 0);
	rcv = (int *)(rcv_ret->addr);
	switch (rcv[0]) {
	case RPC_STREAM:
		rpc_stream_sent();
		break;
	case RPC_RR:
		rpc_rr_sent();
		break;
	default:
		printc("rr %d rcv %d\n", RPC_RR, rcv[0]);
		assert(0);
	}

	return ;
}

void
server_start(int cur)
{
	int msg_sz;
	struct create_ret *crt_ret;

	cur_node = cur;
	printc("I am server node %d meta %p magic %s done %d\n", cur_node, ivshmem_meta, ivshmem_meta->magic, ivshmem_meta->boot_done);
	call_cap_mb(RPC_REGISTER, cur_node, 0, 0);
	msg_sz = round_up_to_page(PACKET_SIZE);
	crt_ret = (struct create_ret *)call_cap_mb(RPC_CREATE, cur_node, msg_sz, 0);
	snt_buf = crt_ret->addr;
	snt_id  = crt_ret->mem_id;
	assert(snt_buf);

	((int *)snt_buf)[0] = test_type;
	call_cap_mb(RPC_SEND, (snt_id << 16) | cur_node, 1-cur_node, sizeof(int));
	switch (test_type) {
	case RPC_STREAM:
		rpc_stream_recv();
		break;
	case RPC_RR:
		rpc_rr_recv();
		break;
	default:
		assert(0);
	}

	return ;
}

