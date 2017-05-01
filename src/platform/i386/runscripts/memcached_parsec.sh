#!/bin/sh

cp memcached_parsec.o llboot.o
./cos_linker "llboot.o, ;llpong.o, :" ./gen_client_stub
