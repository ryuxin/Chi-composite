#ifndef TEST_H
#define TEST_H

#include "micro_booter.h"
#include "server_stub.h"

#define PMEM_TEST 1
#define MEM_SIZE (PGD_SIZE)
enum test_captbl_layout {
	TEST_CT         = 2,
	TEST_PT         = 4,
	TEST_UNTYPED_PT = 6,
	TEST_PMEM_PT    = 8,
	TEST_COMP       = 12,
	TEST_THREAD     = 16,
	TEST_TCAP       = 20,
	TEST_RECV       = 24,
	TEST_RESERVE    = round_up_to_pow2(TEST_RECV+1, CAPMAX_ENTRY_SZ),
	TEST_FREE       = round_up_to_pow2(TEST_RESERVE+1, CAPMAX_ENTRY_SZ)
};

int test_serverfn(int a, int b, int c);
void test_start(int node, vaddr_t untype, vaddr_t vas);
DECLARE_INTERFACE(test_serverfn)
DECLARE_INTERFACE(test_start)

#endif /* TEST_H */
