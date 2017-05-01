#!/bin/sh
if [ $# != 1 ]; then
  echo "Usage: $0 <run-script.sh> <core>"
  exit 1
fi

if ! [ -r $1 ]; then
  echo "Can't open run-script"
  exit 1
fi

MODULES=$(sh $1 | awk '/^Writing image/ { print $3; }' | tr '\n' ' ')
echo $MODULES | tr ' ' ','

