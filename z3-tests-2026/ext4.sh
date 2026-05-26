#!/bin/bash

# (32 * (1 << (n)) * 8192)

umount /mnt/tapedev

set -e

if [[ $# != 1 ]]; then
    echo usage: $0 tapedev_path
    exit -1
fi

dev=$1
echo $dev

size=$(lsblk -b -o size $dev | tail -n1)
size=$((size / (32*32) ))

echo $size

mkdir -p /mnt/tapedev
mkdir -p /tmp/files

mkfs.ext4 -F $dev -D
mount $dev /mnt/tapedev

dd if=/dev/urandom bs=$size count=32 of=/tmp/files/1
dd if=/dev/urandom bs=$size count=32 of=/tmp/files/2
dd if=/dev/urandom bs=$size count=32 of=/tmp/files/3
dd if=/dev/urandom bs=$size count=32 of=/tmp/files/4

mkdir -p /mnt/tapedev/a
mkdir -p /mnt/tapedev/b
mkdir -p /mnt/tapedev/c
mkdir -p /mnt/tapedev/d

cp /tmp/files/* /mnt/tapedev/a
cp /tmp/files/* /mnt/tapedev/b
cp /tmp/files/* /mnt/tapedev/c
cp /tmp/files/* /mnt/tapedev/d

umount /mnt/tapedev

e2fsck -f $dev

mount $dev /mnt/tapedev

cmp /tmp/files/1 /mnt/tapedev/a/1
cmp /tmp/files/2 /mnt/tapedev/a/2
cmp /tmp/files/3 /mnt/tapedev/a/3
cmp /tmp/files/4 /mnt/tapedev/a/4

cmp /tmp/files/1 /mnt/tapedev/b/1
cmp /tmp/files/2 /mnt/tapedev/b/2
cmp /tmp/files/3 /mnt/tapedev/b/3
cmp /tmp/files/4 /mnt/tapedev/b/4

cmp /tmp/files/1 /mnt/tapedev/c/1
cmp /tmp/files/2 /mnt/tapedev/c/2
cmp /tmp/files/3 /mnt/tapedev/c/3
cmp /tmp/files/4 /mnt/tapedev/c/4

cmp /tmp/files/1 /mnt/tapedev/d/1
cmp /tmp/files/2 /mnt/tapedev/d/2
cmp /tmp/files/3 /mnt/tapedev/d/3
cmp /tmp/files/4 /mnt/tapedev/d/4

df -h $dev

umount /mnt/tapedev

