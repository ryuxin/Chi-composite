#include "memcached.h"
#include "test.h"
#include "rpc.h"

#define READ_THPUT 1
#define N_OPS 10000000
//#define N_OPS 2000
#define N_KEYS 1000000
//#define N_KEYS 10
#define BUFFER_SIZE 100

char *ops, *load_key;
static void *buf[NUM_NODE/2];
static int memid[NUM_NODE/2];
static char *mc_key, *mc_data, *mc_ret;
int *req_cost;
int n_read, n_update, n_tot;

int
kernel_flush(void)
{
	int r;
	do {
		r = call_cap(CCFLUSH_CAP_TEMP, 0, 0, 0, 0);
	} while (r < 0);
	return r;
}

static int cmpfunc(const void * a, const void * b)
{
    return ( *(int*)b - *(int*)a );
}

static void
out_latency(void)
{
	qsort(req_cost, n_tot, sizeof(int), cmpfunc);
	printc("tot %d %d 99.9 %d 99 %d min %d max %d\n", n_tot, n_read+n_update, 
		req_cost[n_tot/1000], req_cost[n_tot/100], req_cost[n_tot-1], req_cost[0]);
}

static inline void
disconnect_mc_server(int server)
{
	struct mc_msg *msg = (struct mc_msg *)buf[server];

	msg->type = MC_EXIT;
	call_cap_mb(RPC_SEND, (memid[server] << 16) | cur_node, server, 4);
}

static int
server_set_key(char *key, int nkey, char *data, int nbytes)
{
	uint32_t hv;
	int node, r;
	hv   = hash(key, nkey);
	node = (hv & hashmask(hashpower)) % (NUM_NODE/2);

	r = mc_set_key_ext(node, key, nkey, data, nbytes);
	assert(!r);
	return r;
}

static int
client_get_key(char *key, int nkey)
{
	void *rcv;
	int node;
	uint32_t hv;
	hv   = hash(key, nkey);
	node = (hv & hashmask(hashpower)) % (NUM_NODE/2);

#ifdef GET_RPC_TEST
	int r, msg_sz;
	struct recv_ret *rcv_ret;
	struct mc_msg *msg = (struct mc_msg *)buf[node];
	char *k = (char *)buf[node]+sizeof(struct mc_msg), *d;

	msg_sz    = sizeof(struct mc_msg)+nkey;
	msg->type = MC_MSG_GET;
	msg->nkey = nkey;
	msg->key  = k;
	memcpy(msg->key, key, nkey);
	assert(msg_sz < PAGE_SIZE);

	r = call_cap_mb(RPC_SEND, (memid[node] << 16) | cur_node, node, msg_sz);
	assert(!r);
	rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1, 0);
	assert(rcv_ret);
	assert(rcv_ret->sender == node);
	rcv = rcv_ret->addr;
	assert(rcv);
	if (MC_MSG_GET_OK == ((struct mc_msg *)rcv)->type) {
		assert(((struct mc_msg *)rcv)->nbytes == V_LENGTH);
		assert(((struct mc_msg *)rcv)->data);
		d = (char *)rcv+sizeof(struct mc_msg);
		assert(d[0] == '$');
		return 0;
	} else if (MC_MSG_GET_FAIL == ((struct mc_msg *)rcv)->type) {
		assert(((struct mc_msg *)rcv)->nbytes == 0);
		assert(!((struct mc_msg *)rcv)->data);
		return -1;
	} else {
		assert(0);
	}
#else
	memcpy(mc_key, key, nkey);
	rcv = call_cap_mb(MC_GET_KEY, (node << 16) | cur_node, nkey, 0);
	if (!rcv) return -1;
#endif
	return 0;
}

static int
client_set_key(char *key, int nkey, char *data, int nbytes)
{
	void *rcv;
	int node;
	uint32_t hv;
	hv   = hash(key, KEY_LENGTH);
	node = (hv & hashmask(hashpower)) % (NUM_NODE/2);

#ifdef SET_RPC_TEST
	int r, msg_sz;
	struct recv_ret *rcv_ret;
	struct mc_msg *msg = (struct mc_msg *)buf[node];
	char *k = (char *)buf[node]+sizeof(struct mc_msg);

	msg_sz    = sizeof(struct mc_msg)+nkey+nbytes;
	msg->type   = MC_MSG_SET;
	msg->nkey   = nkey;
	msg->key    = k;
	msg->nbytes = nbytes;
	msg->data   = k+nkey;
	memcpy(msg->key, key, nkey);
	memcpy(msg->data, data, nbytes);
	assert(msg_sz < PAGE_SIZE);

	r = call_cap_mb(RPC_SEND, (memid[node] << 16) | cur_node, node, msg_sz);
	assert(!r);
	rcv_ret = (struct recv_ret *)call_cap_mb(RPC_RECV, cur_node, 1, 0);
	assert(rcv_ret);
	assert(rcv_ret->sender == node);
	rcv = rcv_ret->addr;
	assert(rcv);
	assert(MC_MSG_SET_OK == ((struct mc_msg *)rcv)->type);
	return 0;
#else
	memcpy(mc_key, key, nkey);
	memcpy(mc_data, data, nbytes);
	rcv = call_cap_mb(MC_SET_KEY, (node << 16) | cur_node, nkey, nbytes);
	assert(!rcv);
	return (int)rcv;
#endif
}

static void 
bench()
{
	int i, ret, maxkf = 0;
	char *op = ops, value[V_LENGTH], *key;
	unsigned long long s, e, s1, e1, max = 0, cost;
	unsigned long long tot_cost = 0, tot_r, tot_w, max_r;
	unsigned long long prev = 0;

	/* prepare the value -- no real database op needed. */
	memset(value, '$', V_LENGTH);
	n_read = n_update = n_tot = 0;
	tot_r = tot_w = max_r = 0;

	rdtscll(s);
	for (i = 0; i < N_OPS; i++) {
		assert(op[KEY_LENGTH + 1] == '\n');
		key = &op[1];

		rdtscll(s1);
		if (!prev) prev = s1;
		if (s1-prev > MC_HASH_FLUSH_PEROID) {
#ifndef GET_RPC_TEST
			call_cap_mb(MC_FLUSH, cur_node, 0, 0);
#endif
			ret = kernel_flush();
			if (ret > maxkf) maxkf = ret;
			prev = s1;
		}
		rdtscll(s1);
		if (*op == 'R') {
			ret = client_get_key(key, KEY_LENGTH);
		} else {
			assert(*op == 'U');
			ret = client_set_key(key, KEY_LENGTH, value, V_LENGTH);
			assert(ret == 0);
		}
		rdtscll(e1);
		if (!ret) {
			cost = e1-s1;
			if (*op == 'R') {
				tot_r += cost;
				n_read++;
				if (cost > max_r) max_r = cost;
			}
#ifndef READ_THPUT
			else {
				tot_w += cost;
				n_update++;
			}
#endif

#ifdef READ_THPUT
			if (*op == 'R')
#endif
			{
			tot_cost += cost;
			req_cost[n_tot++] = (int)cost;
			if (cost > max) max = cost;
			}
		}
		op += (KEY_LENGTH+2);
	}
	rdtscll(e);

	printc("Node %d: tot %lu ops (r %lu, u %lu) done, time(ms) %llu, thput %llu\n",
	   	cur_node, n_read+n_update, n_read, n_update, tot_cost/(unsigned long long)CPU_FREQ, 
		   (unsigned long long)CPU_FREQ * n_tot * 1000 / tot_cost);
	printc("%llu (%llu) cycles per op, max %llu read %llu, get %llu, set %llu kernel flush %d\n", (unsigned long long)(e-s)/(n_read + n_update),
	   	tot_cost/(n_read+n_update), max, max_r, tot_r/n_read, tot_w/n_update, maxkf);
	call_cap_mb(MC_PRINT_STATUS, cur_node, KEY_LENGTH, V_LENGTH);
}

void
preload_key(int cur)
{
	/* insert all the keys into the cache before accessing the
	* traces. If the cache is large enough, there will be no miss.*/
	char *buf = load_key, v[V_LENGTH];
	int bytes = KEY_LENGTH + 1, i;
	uint32_t hv;
	unsigned long long start, end;

	rdtscll(start);
	for (i = 0; i < N_KEYS; i++) {
		memcpy(v, buf, KEY_LENGTH);
		memcpy(&v[KEY_LENGTH], buf, KEY_LENGTH);
		memset(v, '$', V_LENGTH);
		server_set_key(buf, KEY_LENGTH, v, V_LENGTH);
		buf += bytes;
	}
	rdtscll(end);
	printc("load key node finish total key %d avg %llu\n", N_KEYS, (end-start)/N_KEYS);
	mc_print_status();
}

int
client_start(int cur)
{
	int i=0;

	assert(cur >= NUM_NODE/2);
	cos_faa(&ivshmem_meta->boot_done, 1);
	while (*(volatile int *)&(ivshmem_meta->boot_done) != 1+NUM_NODE) { ; }
	printc("client %d bengin bench done %d\n", cur, ivshmem_meta->boot_done);
	i = kernel_flush();
	printc("flush kernel page %d\n", i);
	bench();
	cos_faa(&ivshmem_meta->boot_num, 1);
	i = kernel_flush();
	out_latency();
	printc("flush kernel page %d\n", i);
	while (*(volatile int *)&(ivshmem_meta->boot_num) != NUM_NODE*3/2) { ; }
	disconnect_mc_server(0);
	disconnect_mc_server(1);
	return i;
}

void
client_init(int cur)
{
	struct create_ret *crt_ret;
	int msg_sz = PAGE_SIZE, i;

	req_cost = (int *)((char *)cos_get_heap_ptr()+PAGE_SIZE);
	assert(cur >= NUM_NODE/2);
	cur_node = cur;
	printc("I am client heap %p cur_node %d\n", cos_get_heap_ptr(), cur_node);
	printc("meta %p magic %s done %d\n", ivshmem_meta, ivshmem_meta->magic, ivshmem_meta->boot_done);
	call_cap_mb(RPC_REGISTER, cur_node, 2, 3);
	for(i=0; i<NUM_NODE/2; i++) {
		crt_ret = (struct create_ret *)call_cap_mb(RPC_CREATE, cur_node, msg_sz, 3);
		assert(crt_ret);
		buf[i]   = crt_ret->addr;
		memid[i] = crt_ret->mem_id;
		assert(buf[i]);
	}

	printc("client-mc set up share page node %d\n", cur);
	mc_key  = (char *)call_cap_mb(MC_REGISTER, cur_node, 1, 1);
	assert(mc_key);
	mc_data = (char *)call_cap_mb(MC_REGISTER, cur_node, 2, 1);
	assert(mc_data);
	mc_ret  = (char *)call_cap_mb(MC_REGISTER, cur_node, 3, 1);
	assert(mc_ret);

	hash_init(JENKINS_HASH);
}
