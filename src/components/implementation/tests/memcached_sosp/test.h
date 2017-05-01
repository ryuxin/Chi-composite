#ifndef TEST_H
#define TEST_H

#include "micro_booter.h"
#include "server_stub.h"

#define MC_LOADING 99

typedef enum {
	MC_MSG_GET,
	MC_MSG_SET,
	MC_MSG_GET_OK,
	MC_MSG_GET_FAIL,
	MC_MSG_SET_OK,
	MC_MSG_SET_FAIL,
	MC_MEM_REQ,
	MC_MEM_REPLY,
	MC_BEGIN,
	MC_EXIT
} mc_message_t;

struct mc_msg {
	mc_message_t type;
	int id, nkey, nbytes;
	uint32_t hv;
	char *key, *data;
};

extern char *ops;
int kernel_flush(void);
void client_load(int cur);
void client_bench(int cur);
void client_init(int cur);
int client_start(int cur);
DECLARE_INTERFACE(client_init)
DECLARE_INTERFACE(client_start)

#endif /* TEST_H */
