#include "server_stub.h"

#define RET_CAP (1 << 16)
/* Use the passed in stack in arg4 */

.text
IMPL_INTERFACE(client_start)
IMPL_INTERFACE(server_start)
IMPL_INTERFACE(rpc_create)
IMPL_INTERFACE(rpc_connect)
IMPL_INTERFACE(rpc_send)
IMPL_INTERFACE(rpc_recv)
IMPL_INTERFACE(rpc_call)
IMPL_INTERFACE(rpc_wait_replay)
IMPL_INTERFACE(rpc_free)
IMPL_INTERFACE(rpc_register)
IMPL_INTERFACE(rpc_init)
