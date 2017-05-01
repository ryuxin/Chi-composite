C_OBJS=micro_booter.o rpc.o mem_mgr.o assoc.o hash.o items.o jenkins_hash.o memcache.o test.o parsec.o ps_slab.o
ASM_OBJS=cos_asm_scheduler.o inv.o
COMPONENT=memcached_sosp.o
INTERFACES=
DEPENDENCIES=
IF_LIB=./trace.o
ADDITIONAL_LIBS=-lcobj_format -lcos_kernel_api

pre: first all
first:
	ld -r -b binary -o trace.o preload_key trace_key

include ../../Makefile.subsubdir
MANDITORY_LIB=simple_stklib.o
