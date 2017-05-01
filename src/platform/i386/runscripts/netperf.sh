#!/bin/sh

cp netperf.o llboot.o
./cos_linker "llboot.o, ;llpong.o, :" ./gen_client_stub
