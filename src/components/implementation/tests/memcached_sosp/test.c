#include "memcached.h"
#include "test.h"
#include "rpc.h"

#define N_KEYS 4000000
#define N_OPS  10000000

char *ops, *load_key;;
static char *mc_key, *mc_data, *mc_ret;
int *req_cost;

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

static inline void
disconnect_mc_server(int cur, int server)
{
	call_cap_mb(MC_DISCONNECT, cur, server, 4);
}

static int
client_get_key(char *key, int nkey)
{
	void *rcv;

#ifdef MC_IPC
	memcpy(mc_key, key, nkey);
	rcv = (void *)call_cap_mb(MC_GET_KEY, cur_node, nkey, 0);
#else
	int nbyte;
	rcv = (void *)mc_get_key_ext(key, nkey, &nbyte);
#endif
	if (!rcv) return -1;
	assert(((char *)rcv)[0] == '$');
	return 0;
}

static int
client_set_key(int cur, char *key, int nkey, char *data, int nbytes)
{
	void *rcv;

#ifdef MC_IPC
	cos_memcpy(mc_key, key, nkey);
	cos_memcpy(mc_key+nkey, data, nbytes);
	rcv = (void *)call_cap_mb(MC_SET_KEY, cur, nkey, nbytes);
#else
	int node, r;
	uint32_t hv;

	hv   = hash(key, nkey);
	node = __mc_hv2node(hv);
	rcv = (void *)mc_set_key_ext(cur, node, key, nkey, data, nbytes, hv);
#endif
#ifndef KEY_SKEW
	assert(!rcv);
#endif
	return (int)rcv;
}

void 
client_bench(int cur)
{
	int i, ret, id = cur - NUM_NODE/2;
	char *op = ops, value[V_LENGTH], *key;
	unsigned long long s, e, s1, e1, cost;
	unsigned long long tot_cost = 0, tot_r, tot_w;
	unsigned long long prev = 0;
	int n_read, n_update, n_tot, nget;

#ifndef SET_RPC_TEST
	unsigned long long flush_t = 0;
	int flush_n = 0;
#endif
	/* prepare the value -- no real database op needed. */
	memset(value, '$', V_LENGTH);
	tot_r = tot_w = 0;
	n_read = n_update = n_tot = nget = 0;
	if (id) op += (KEY_LENGTH+2);

	rdtscll(s);
	for (i = 0; i < N_OPS; i+= (NUM_NODE/2)) {
		assert(op[KEY_LENGTH + 1] == '\n');
		key = &op[1];

		rdtscll(s1);
		if (!prev) prev = s1;
		if (s1-prev > MC_HASH_FLUSH_PEROID) {
#ifndef SET_RPC_TEST
			rdtscll(s1);
#endif
#ifdef MC_IPC
			call_cap_mb(MC_FLUSH, cur, 0, 0);
#else
			mc_hashtbl_flush(cur);
#endif
			ret = kernel_flush();
			prev = s1;
#ifndef SET_RPC_TEST
			rdtscll(e1);
			assert(e1 > s1);
			flush_t += (e1-s1);
			flush_n++;
#endif
		}

                rdtscll(s1);
                if (*op == 'R') {
                        n_read++;
                        ret = client_get_key(key, KEY_LENGTH);
                        rdtscll(e1);
                        cost = e1-s1;
                        assert(e1 > s1);
                        tot_r += cost;
                } else {
                        assert(*op == 'U');
                        n_update++;
                        ret = client_set_key(cur, key, KEY_LENGTH, value, V_LENGTH);
                        assert(ret == 0);
                        rdtscll(e1);
                        cost = e1-s1;
                        assert(e1 > s1);
                        tot_w += cost;
                }
                tot_cost += cost;
                if (!ret) {
                        if (*op == 'R') nget++;
			n_tot++;
                }
		op += (KEY_LENGTH+2)*(NUM_NODE/2);
	}
	rdtscll(e);

	printc("Node %d: tot %d ops (r %d, u %d) done, time(ms) %llu, thput %llu\n",
	       cur, n_read+n_update, n_read, n_update, (e - s)/(unsigned long long)CPU_FREQ, 
	       (unsigned long long)CPU_FREQ * n_tot * 1000 / (e - s));
	unsigned long long rl, ul;
	if (n_read == 0) rl = 0;
	else rl = tot_r/n_read;
	if (n_update == 0) ul = 0;
	else ul = tot_w/n_update;
        printc("%llu (%llu) op, get %llu, set %llu miss %d (%%%%) %d\n", (unsigned long long)(e-s)/(n_read + n_update),
	       tot_cost/(n_read+n_update), rl, ul, n_read - nget, (n_read ? (n_read - nget) * 1000 / n_read : 0));

#ifdef MC_IPC
	call_cap_mb(MC_PRINT_STATUS, cur_node, KEY_LENGTH, V_LENGTH);
#else
	mc_print_status();
#endif
#ifndef SET_RPC_TEST
	printc("flush %d (%%) %llu\n", flush_n, flush_t*100/(e - s));
#endif
}

void
client_load(int cur)
{
	/* insert all the keys into the cache before accessing the
	* traces. If the cache is large enough, there will be no miss.*/
	char *buf = load_key, v[V_LENGTH+1];
	int bytes = KEY_LENGTH + 1, i, r;
	unsigned long long start, end;

	memset(v, '$', V_LENGTH);
	rdtscll(start);
	for (i = 0; i < N_KEYS; i++) {
		r = client_set_key(cur, buf, KEY_LENGTH, v, V_LENGTH);
		if (r) break;
		buf += bytes;
	}
	rdtscll(end);
	printc("load key node finish total key %d avg %llu\n", i, (end - start)/(unsigned long long)i);
}

void
preload_key(int cur)
{
#ifdef MC_IPC
	client_load(cur);
#else
	call_cap_mb(MC_PRELOAD, cur, 0, 0);
#endif
	disconnect_mc_server(cur, MC_LOADING);
	call_cap_mb(MC_PRINT_STATUS, cur, KEY_LENGTH, V_LENGTH);
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
#ifdef MC_IPC
	client_bench(cur_node);
#else
	call_cap_mb(MC_TEST, cur_node, 0, 0);
#endif
	cos_faa(&ivshmem_meta->boot_num, 1);
	i = kernel_flush();
	printc("flush kernel page %d\n", i);
	while (*(volatile int *)&(ivshmem_meta->boot_num) != NUM_NODE*3/2) { ; }
	disconnect_mc_server(cur_node, cur_node-NUM_NODE/2);
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

#ifdef MC_IPC
	printc("client-mc set up share page node %d\n", cur);
	mc_key  = (char *)call_cap_mb(MC_REGISTER, cur_node, 1, 1);
	assert(mc_key);
	mc_data = (char *)call_cap_mb(MC_REGISTER, cur_node, 2, 1);
	assert(mc_data);
	mc_ret  = (char *)call_cap_mb(MC_REGISTER, cur_node, 3, 1);
	assert(mc_ret);
#endif
	if (cur == NUM_NODE/2) preload_key(cur);
	printc("%d client init finish\n", cur);
}
