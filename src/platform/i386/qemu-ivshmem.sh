#!/bin/sh
# mount hugepage first: mount -t hugetlbfs hugetlbfs /libhugetlbfs
#sudo chmod a+wr /libhugetlbfs/
if [ $# != 2 ]; then
  echo "Usage: $0 <module> <core>"
  exit 1
fi

taskset -c $2 /home/ryx/Downloads/qemu-2.7.0/build/i386-softmmu/qemu-system-i386 --enable-kvm \
-m 356 -mem-path /libhugetlbfs \
-object memory-backend-file,size=1024M,share,mem-path=/libhugetlbfs/ivshmem,id=md1 \
-device ivshmem-plain,memdev=md1 \
-cpu host -nographic -kernel kernel.img -no-reboot -initrd $1

