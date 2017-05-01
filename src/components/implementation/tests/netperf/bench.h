#ifndef BENCH_H
#define BENCH_H

#include "micro_booter.h"
#include "server_stub.h"

void client_start(int cur);
void server_start(int cur);
DECLARE_INTERFACE(client_start)
DECLARE_INTERFACE(server_start)

#endif /* BENCH_H */
