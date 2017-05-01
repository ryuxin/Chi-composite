#!/bin/sh

cp microbench.o llboot.o
./cos_linker "llboot.o, ;llpong.o, :" ./gen_client_stub
