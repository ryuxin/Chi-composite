#!/bin/sh

cp memcached_sosp.o llboot.o
./cos_linker "llboot.o, ;llpong.o, :" ./gen_client_stub
