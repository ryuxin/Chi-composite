#ifndef TEST_H
#define TEST_H

#include "micro_booter.h"
#include "server_stub.h"

typedef enum {
	MC_MSG_GET,
	MC_MSG_SET,
	MC_MSG_GET_OK,
	MC_MSG_GET_FAIL,
	MC_MSG_SET_OK,
	MC_MSG_SET_FAIL,
	MC_MEM_REQ,
	MC_MEM_REPLY,
	MC_EXIT
} mc_message_t;

struct mc_msg {
	mc_message_t type;
	int nkey, nbytes;
	char *key, *data;
};

extern char *ops;
extern char *load_key;
int kernel_flush(void);
void client_init(int cur);
int client_start(int cur);
void preload_key(int cur);
DECLARE_INTERFACE(client_init)
DECLARE_INTERFACE(client_start)
DECLARE_INTERFACE(preload_key)

#endif /* TEST_H */
