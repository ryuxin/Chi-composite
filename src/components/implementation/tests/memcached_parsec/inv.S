#include "server_stub.h"

#define RET_CAP (1 << 16)
/* Use the passed in stack in arg4 */

.text
IMPL_INTERFACE(client_init)
IMPL_INTERFACE(client_start)
IMPL_INTERFACE(preload_key)
IMPL_INTERFACE(rpc_create)
IMPL_INTERFACE(rpc_connect)
IMPL_INTERFACE(rpc_send)
IMPL_INTERFACE(rpc_recv)
IMPL_INTERFACE(rpc_free)
IMPL_INTERFACE(rpc_register)
IMPL_INTERFACE(rpc_init)
IMPL_INTERFACE(mc_register)
IMPL_INTERFACE(mc_set_key)
IMPL_INTERFACE(mc_get_key)
IMPL_INTERFACE(mc_init)
IMPL_INTERFACE(mc_server_start)
IMPL_INTERFACE(mc_print_status)
IMPL_INTERFACE(mc_hashtbl_flush)
IMPL_INTERFACE(mc_mem_balance)
