#!/bin/bash

shopt -s nullglob

path=$(realpath $0)
dir=$(dirname $path)

i=0
for t in /dev/tapedev*; do
    echo $t
    bytes=$(lsblk -b --output SIZE -n $t)
    no=$(echo $t | sed 's/\/dev\/tapedev//g')
    dd if=/dev/urandom of=urandom_$no count=$((bytes / 512)) ibs=512 &
    i=$((i+1))
done

wait $(jobs -p)

echo "step: writing"

for t in /dev/tapedev*; do
    bytes=$(lsblk -b --output SIZE -n $t)
    no=$(echo $t | sed 's/\/dev\/tapedev//g')
    dd if=urandom_$no of=$t bs=8333 status=progress &
done

wait $(jobs -p)

echo "step: reading"

for t in /dev/tapedev*; do
    no=$(echo $t | sed 's/\/dev\/tapedev//g')
    dd if=$t of=output_$no bs=8333 status=progress &
done

wait $(jobs -p)

for t in /dev/tapedev*; do
    no=$(echo $t | sed 's/\/dev\/tapedev//g')
    cmp urandom_$no output_$no 
done
